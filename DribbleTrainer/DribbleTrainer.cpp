#include "DribbleTrainer.h"
#include "bakkesmod\wrappers\includes.h"
#include <time.h>
#include <ctime>
#include <cstdlib>

using namespace std::chrono;

BAKKESMOD_PLUGIN(DribbleTrainer, "Freeplay training for dribbles, flicks, and catches", "1.0", PLUGINTYPE_FREEPLAY)

/*
TO-DO

    - CATCH
        - Add velocity prediction
        - LEFT OFF HERE: https://www.youtube.com/watch?v=RhUdv0jjfcE&list=PLqwfRVlgGdFAZeT_o-ASucXoFsajXMirw&index=12

*/

void DribbleTrainer::onLoad()
{
    srand(clock());

    //Notifiers
    cvarManager->registerNotifier(NOTIFIER_RESET,        [this](std::vector<std::string> params){Reset();}, "Resets ball to dribbling position", PERMISSION_ALL);
    cvarManager->registerNotifier(NOTIFIER_LAUNCH,       [this](std::vector<std::string> params){GetNextLaunchDirection();}, "Launch the ball toward your car for catch practice", PERMISSION_ALL);
    cvarManager->registerNotifier(NOTIFIER_REQUEST_MODE, [this](std::vector<std::string> params){RequestToggle(params);}, "Launch the ball toward your car for catch practice", PERMISSION_ALL);
    
    //Sliders
    angularReduction  = std::make_shared<float>(0.f);
    floorThreshold    = std::make_shared<float>(0.f);
    maxFlickDistance  = std::make_shared<float>(0.f);
    preparationTime   = std::make_shared<float>(0.f);
    catchSpreadAmount = std::make_shared<float>(0.f);
    cvarManager->registerCvar(CVAR_ANGULAR_REDUCTION,   "0.5",          "How much the angular velocity should be reduced on reset", true, true, 0,   true, 1).bindTo(angularReduction);
    cvarManager->registerCvar(CVAR_BALL_FLOOR_HEIGHT,   "2",            "How close the ball can get to the floor before resetting", true, true, 0,   true, 100000).bindTo(floorThreshold);
    cvarManager->registerCvar(CVAR_BALL_MAX_DISTANCE,   "1250",         "Max distance the ball can move before resetting to car",   true, true, 300, true, 100000).bindTo(maxFlickDistance);
    cvarManager->registerCvar(CVAR_CATCH_PREPARATION,   "2",            "Preparation time before launching",    true, true, 0,  true, 20).bindTo(preparationTime);
    cvarManager->registerCvar(CVAR_CATCH_SPEED,         "(1500, 3500)", "Launch speed randomization range",     true, true, 0,  true, 5000);
    cvarManager->registerCvar(CVAR_CATCH_ANGLE,         "(15, 75)",     "Launch angle randomization range",     true, true, 10, true, 90);
    cvarManager->registerCvar(CVAR_CATCH_SPREAD,        "100",          "Random radius for ball target spread", true, true, 0,  true, 500).bindTo(catchSpreadAmount);
    
    //Bools
    bEnableDribbleMode  = std::make_shared<bool>(false);
    bEnableFlicksMode   = std::make_shared<bool>(false);
    bShowSafeZone       = std::make_shared<bool>(false);
    bShowFloorHeight    = std::make_shared<bool>(false);
    bLogFlickSpeed      = std::make_shared<bool>(false);
    bShowTargetLocation = std::make_shared<bool>(false);
    cvarManager->registerCvar(CVAR_TOGGLE_DRIBBLE_MODE,  "0", "Reset the ball if it falls below floor height").bindTo(bEnableDribbleMode);
    cvarManager->registerCvar(CVAR_TOGGLE_FLICKS_MODE,   "0", "Reset the ball if it goes farther than max flick distance").bindTo(bEnableFlicksMode);
    cvarManager->registerCvar(CVAR_SHOW_SAFE_ZONE,       "1", "Show where the ball should be to keep it balanced").bindTo(bShowSafeZone);
    cvarManager->registerCvar(CVAR_SHOW_FLOOR_HEIGHT,    "0", "Show where the reset threshold is for dribbling").bindTo(bShowFloorHeight);
    cvarManager->registerCvar(CVAR_LOG_FLICK_SPEED,      "1", "Save flick speed to bakkesmod.log so you can see them later").bindTo(bLogFlickSpeed);
    cvarManager->registerCvar(CVAR_SHOW_TARGET_LOCATION, "1", "Show the targeted location in Catch mode").bindTo(bShowTargetLocation);

    bDebugMode = std::make_shared<bool>(false);
    cvarManager->registerCvar(CVAR_DEBUG_MODE, "0", "Draw debug option").bindTo(bDebugMode);

    gameWrapper->RegisterDrawable(bind(&DribbleTrainer::Render, this, std::placeholders::_1));

    gameWrapper->HookEvent("Function TAGame.Ball_TA.Explode", [&](std::string eventName){IsBallHidden = true;});
    gameWrapper->HookEvent("Function GameEvent_Soccar_TA.Active.StartRound", [&](std::string eventName){IsBallHidden = false;}); //Function TAGame.GameEvent_Soccar_TA.StartNewRound
}
void DribbleTrainer::onUnload() {}

//Utility
bool DribbleTrainer::ShouldRun()
{
    if(!gameWrapper->IsInFreeplay()) { return false; }

    ServerWrapper server = gameWrapper->GetGameEventAsServer();
    if(server.IsNull()) { return false; }

    BallWrapper ball = server.GetBall();
    CarWrapper car = gameWrapper->GetLocalCar();
    if(ball.IsNull() || car.IsNull()) { return false; }

    if(IsBallHidden) { return false; }

    return true;
}

void DribbleTrainer::RequestToggle(std::vector<std::string> params)
{
    if(params.size() != 2 || !gameWrapper->IsInFreeplay())
        return;

    if(params.at(1) == "dribble")
    {
        cvarManager->getCvar(CVAR_TOGGLE_DRIBBLE_MODE).setValue(!(*bEnableDribbleMode));
    }
    if(params.at(1) == "flick")
    {
        cvarManager->getCvar(CVAR_TOGGLE_FLICKS_MODE).setValue(!(*bEnableFlicksMode));
    }
}

//Reset
void DribbleTrainer::Reset()
{
    if(!ShouldRun()) { return; }

    ServerWrapper server = gameWrapper->GetGameEventAsServer();
    BallWrapper ball = server.GetBall();
    CarWrapper car = gameWrapper->GetLocalCar();
    RT::Matrix3 carMat(car.GetRotation());
    
    //Don't reset ball if reset location is inside any goal
    ArrayWrapper<GoalWrapper> goals = server.GetGoals();
    for(int i = 0; i < goals.Count(); ++i)
    {
        GoalWrapper goal = goals.Get(i);
        if(goal.memory_address == NULL) { continue; }
        if(IsInGoal(goal, car.GetLocation() + ballResetLocation)) { return; }
    }
    
    //Apply angular velocity reduction
    Vector ballAngular = ball.GetAngularVelocity() * (1 - *angularReduction);

    //Vector positionOffset = (carMat.forward * ballResetPosFwd) + (carMat.right * ballResetPosRight);
    //positionOffset.Z = ballResetPosZ;
    ball.SetLocation(car.GetLocation() + ballResetLocation);
    ball.SetVelocity(car.GetVelocity() + ballResetVelocity);
    ball.SetAngularVelocity(ballAngular, false);
}

bool DribbleTrainer::IsInGoal(GoalWrapper goal, Vector location)
{
    Vector gLoc = goal.GetWorldCenter();
    Vector gSize = goal.GetWorldExtent();

    if(location.X > gLoc.X + gSize.X) { return false; }
    if(location.X < gLoc.X - gSize.X) { return false; }
    if(location.Y > gLoc.Y + gSize.Y) { return false; }
    if(location.Y < gLoc.Y - gSize.Y) { return false; }
    if(location.Z > gLoc.Z + gSize.Z) { return false; }
    if(location.Z < gLoc.Z - gSize.Z) { return false; }

    return true;
}

//Tick
void DribbleTrainer::Tick()
{
    //Called inside the Render function

    if(!ShouldRun()) { return; }

    ServerWrapper server = gameWrapper->GetGameEventAsServer();
    BallWrapper ball = server.GetBall();
    CarWrapper car = gameWrapper->GetLocalCar();

    //Get the ball reset position and velocity
    GetResetValues(ball, car);

    //DRIBBLE MODE
    //If dribble mode is active and ball falls below threshold, reset ball
    if(*bEnableDribbleMode)
    {
        float ballHeight = ball.GetLocation().Z - (ball.GetRadius() + *floorThreshold);
        if(ballHeight <= 0)
        {
            Reset();
        }
    }

    //FLICK MODE
    //If ball is farther than threshold distance, reset ball
    if(*bEnableFlicksMode && !IsBallHidden && abs(car.GetLocation().Y) < 5120)
    {
        float flickDistance = (ball.GetLocation() - car.GetLocation()).magnitude();
        if(flickDistance > *maxFlickDistance)
        {
            int ballSpeed = static_cast<int>(ball.GetVelocity().magnitude() * 0.036f);//cms to kph
            cvarManager->log("Flick Speed: " + std::to_string(ballSpeed) + " KPH");

            //If flick speed logging is enabled, log speed via in-game chat
            if(*bLogFlickSpeed)
            {
                gameWrapper->LogToChatbox(std::to_string(ballSpeed) + " KPH", "Flick Speed");
            }

            //Reset the ball
            Reset();
        }
    }

    //CATCH MODE
    //If the launch timer is counting down, hold the ball in the air from its launch point
    if(preparingToLaunch)
    {
        HoldBallInLaunchPosition(ball, car);
    }
}

void DribbleTrainer::GetResetValues(BallWrapper ball, CarWrapper car)
{
    RT::Matrix3 carMat(car.GetRotation());

    Vector carAcceleration = GetAcceleration(car);
    Vector carVelocity = car.GetVelocity();
    Vector carAngular = car.GetAngularVelocity();

    float ForwardAcceleration = Vector::dot(carMat.forward, carAcceleration);//range -150 to 150

    float speedPerc = carVelocity.magnitude() / 2300;
    float angularPerc = abs(carAngular.Z) / 5.5f;
    Vector forwardOffset = carMat.forward * ForwardAcceleration * 4.f * speedPerc;
    Vector rightOffset = carMat.right * (350.f * angularPerc * speedPerc);
    Vector spawnOffset = {0, 0, 150};
    Vector velocityAdjust = {0, 0, 0};
    
    if(car.IsOnGround())
    {
        // Handle spawn location when on the ground //

        if(carAngular.Z > 0.f) //right turn
        {
            spawnOffset += rightOffset;
        }
        else if(carAngular.Z < 0.f) //left turn
        {
            spawnOffset -= rightOffset;
        }

        if(abs(carAngular.Z) > 0.f)
        {
            spawnOffset.Z *= (1 - angularPerc * .75f);
            forwardOffset -= (forwardOffset * angularPerc);
            forwardOffset -= (carMat.forward * 200.f * min(angularPerc, 1.f) * speedPerc);
            velocityAdjust -= (carMat.right * Vector::dot(carVelocity, carMat.right));
            velocityAdjust /= 1.5f;
        }

        constexpr float maxVelocityAdjust = 100;
        if(velocityAdjust.magnitude() > maxVelocityAdjust)
        {
            Vector velocityAdjustDirection = velocityAdjust; 
            velocityAdjustDirection.normalize();
            velocityAdjust = velocityAdjustDirection * maxVelocityAdjust;
        }
    }
    else
    {
        // Handle spawn location when in the air //

        spawnOffset += (carVelocity.getNormalized() * 40 + carMat.forward * 25);
    }

    //Reduce the forward offset amount based on the speed of the car, with a minimum forward position
    forwardOffset -= (forwardOffset * speedPerc);

    //Add some forward offset to start momentum when the car is very slow
    float forwardOffsetPerc = (1 - speedPerc) * 1.f + speedPerc * .2f;
    Vector slowForwardOffset = carMat.forward * 30 * forwardOffsetPerc;
    forwardOffset += slowForwardOffset;

    //Make sure ball doesn't spawn in the ground
    spawnOffset.Z = max(spawnOffset.Z, ball.GetRadius());

    //Create a buffer to hold reset values (buffer is created only once)
    static std::vector<ResetData> FinalResetVals;

    //Add reset value to buffer and trim buffer to allotted time
    ResetData FinalResetValue = {spawnOffset + forwardOffset, steady_clock::now()};
    FinalResetVals.push_back(FinalResetValue);
    TrimResetDataBuffer(FinalResetVals, .25f);

    //Assign final values to plugin member variables
    ballResetLocation = GetResetDataBufferAverage(FinalResetVals);
    ballResetLocation.Z = spawnOffset.Z;
    ballResetVelocity = velocityAdjust;
}

Vector DribbleTrainer::GetAcceleration(CarWrapper car)
{
    //Statics - these assignments only happen once
    static Vector previousVelocity = car.GetVelocity();
    static steady_clock::time_point previousTime = steady_clock::now();

    //Get the change in velocity between the last frame and this frame
    Vector velocityChange = car.GetVelocity() - previousVelocity;
    previousVelocity = car.GetVelocity();

    //Get the change in time between the last frame and this frame
    float timeChange = duration_cast<duration<float>>(steady_clock::now() - previousTime).count();
    previousTime = steady_clock::now();

    //Calculate the acceleration from the change in velocity and time
    return (velocityChange * 0.036f) * (1.f / timeChange);
}

void DribbleTrainer::TrimResetDataBuffer(std::vector<ResetData>& Buffer, float MaxBufferTime)
{
    //Loop until buffer is constrained to MaxBufferTime (in seconds)
    while(true)
    {
        auto FirstElementTime = Buffer[0].captureTime;
        auto LastElementTime = Buffer[Buffer.size()-1].captureTime;
        float BufferDuration = duration_cast<duration<float>>(LastElementTime - FirstElementTime).count();
        if(BufferDuration > MaxBufferTime)
        {
            //Erase the front element of the buffer if it is too old
            Buffer.erase(Buffer.begin());
        }
        else
        {
            //Buffer has successfully been constrained
            return;
        }
    }
}

Vector DribbleTrainer::GetResetDataBufferAverage(const std::vector<ResetData>& Values)
{
    Vector result = 0;
    for(const auto& resetValue : Values)
    {
        result += resetValue.location;
    }
    result /= static_cast<float>(Values.size());

    return result;
}


//Catch
void DribbleTrainer::GetNextLaunchDirection()
{
    if(!ShouldRun()) return;

    //CarWrapper car = gameWrapper->GetLocalCar();
    //if(car.IsNull()) return;
    //
    //RT::Matrix3 launchDirectionMatrix;
    //
    ////Rotate around UP axis
    //float UpRotAmount = (static_cast<float>(rand()) / RAND_MAX) * (2.f * CONST_PI_F);
    //Quat upRotQuat = RT::AngleAxisRotation(UpRotAmount, launchDirectionMatrix.up);
    //launchDirectionMatrix.RotateWithQuat(upRotQuat);
    //
    ////Rotate around right axis
    //float launchAngle = cvarManager->getCvar(CVAR_CATCH_ANGLE).getFloatValue();
    //float RightRotAmount = launchAngle * -(CONST_PI_F / 180);
    //Quat rightRotQuat = RT::AngleAxisRotation(RightRotAmount, launchDirectionMatrix.right);
    //launchDirectionMatrix.RotateWithQuat(rightRotQuat);
    //
    //Vector spawnDirection = launchDirectionMatrix.forward;
    //spawnDirection.normalize();
    //
    //float launchSpeed = cvarManager->getCvar(CVAR_CATCH_SPEED).getFloatValue();
    //nextLaunch.launchDirection = spawnDirection;
    //nextLaunch.launchMagnitude = launchSpeed / 5000;

    nextLaunch.launchDirection = GetRandomDirection(-180, 180, -cvarManager->getCvar(CVAR_CATCH_ANGLE).getFloatValue(), -cvarManager->getCvar(CVAR_CATCH_ANGLE).getFloatValue());//spawnDirection;
    nextLaunch.launchMagnitude = cvarManager->getCvar(CVAR_CATCH_SPEED).getFloatValue() / 5000;
    nextLaunch.spreadLocation = GetRandomDirection(-180,180,-90,90) * *catchSpreadAmount;

    PrepareToLaunch();
}

Vector DribbleTrainer::GetRandomDirection(float minHorizontalAngle, float maxHorizontalAngle, float minVerticalAngle, float maxVerticalAngle)
{
    // Swap mins and maxes if they are incorrect
    if(minHorizontalAngle > maxHorizontalAngle)
    {
        std::swap(minHorizontalAngle, maxHorizontalAngle);
    }
    if(minVerticalAngle > maxVerticalAngle)
    {
        std::swap(minVerticalAngle, maxVerticalAngle);
    }

    //Convert parameters from degrees to radians
    constexpr float DegToRad = CONST_PI_F / 180;
    minHorizontalAngle *= DegToRad;
    maxHorizontalAngle *= DegToRad;
    minVerticalAngle   *= DegToRad;
    maxVerticalAngle   *= DegToRad;

    //Generate 2 random values from 0 - 1
    float RandPerc1 = static_cast<float>(rand()) / RAND_MAX;
    float RandPerc2 = static_cast<float>(rand()) / RAND_MAX;

    //Create rotation matrix
    RT::Matrix3 directionMat;

    //Rotate around UP axis - horizontal rotation
    float horizontalRotAmount = ((1 - RandPerc1) * minHorizontalAngle) + (RandPerc1 * maxHorizontalAngle);
    Quat horizontalRotation = RT::AngleAxisRotation(horizontalRotAmount, directionMat.up);
    directionMat.RotateWithQuat(horizontalRotation);

    //Rotate around RIGHT axis - vertical rotation
    float verticalRotAmount = ((1 - RandPerc2) * minVerticalAngle) + (RandPerc2 * maxVerticalAngle);
    Quat verticalRotation = RT::AngleAxisRotation(verticalRotAmount, directionMat.right);
    directionMat.RotateWithQuat(verticalRotation);
    
    return directionMat.forward.getNormalized();
}

float DribbleTrainer::GetRandomPercent(float minVal, float maxVal)
{
    if(minVal < 0 || minVal > 1 || maxVal < 0 || maxVal > 1) { return 0; }
    if(minVal > maxVal) { std::swap(minVal, maxVal); }


}

void DribbleTrainer::PrepareToLaunch()
{
    preparingToLaunch = true;
    preparationStartTime = clock();
    ++launchNum;
    gameWrapper->SetTimeout(std::bind(&DribbleTrainer::Launch, this, launchNum), *preparationTime);
}

void DribbleTrainer::HoldBallInLaunchPosition(BallWrapper ball, CarWrapper car)
{
    //Called in Tick

    Vector spawnLocation = car.GetLocation() + nextLaunch.launchDirection * (min(*maxFlickDistance, 2000) - 150.f);

    ball.SetVelocity(Vector{0,0,0});
    ball.SetLocation(GetSafeHoldPosition(spawnLocation));
}

Vector DribbleTrainer::GetSafeHoldPosition(Vector InLocation)
{
    //Prevent HoldBallInLaunchPosition() from holding the ball outside the arena
    //Has issues when sitting in the corner but that's not worth the trouble of fixing

    //Arena height
    if(InLocation.Z <  100)  { InLocation.Z =  100;  }
    if(InLocation.Z >  1900) { InLocation.Z =  1900; }

    //Arena width
    if(InLocation.X < -4000) { InLocation.X = -4000; }
    if(InLocation.X >  4000) { InLocation.X =  4000; }

    //Arena length
    if(InLocation.Y < -5000) { InLocation.Y = -5000; }
    if(InLocation.Y >  5000) { InLocation.Y =  5000; }

    return InLocation;
}

void DribbleTrainer::Launch(int launchIndex)
{
    //Launch the ball at roughly the speed of the car
    //Use redirectplugin code for this
    //https://github.com/bakkesmodorg/BakkesMod2-Plugins/blob/master/RedirectPlugin/redirectplugin.cpp

    //Calculate launch velocity to make it catchable
    //BALL VELOCITY TO MEET CAR

    /*
    velMultiplied = playerVelocity * Vector(predictMultiplierX, predictMultiplierY, 1);
    offsetVec = offsetVec + velMultiplied;
    Vector shotData = training.GenerateShot(ballPosition, playerPosition + offsetVec, ballSpeed);
    if (onGround)
        shotData.Z = 0;
    training.GetBall().SetVelocity(shotData);
    */

    /*
    Vector ServerWrapper::GenerateShot(Vector startPos, Vector destination, float speed)
    {
        //speed = range from 0 to 2000
        speed = 2000 - speed;
        Vector shotVelocity = Vector(0);

        Vector destination = car.GetLocation();
        Vector startPos = ball.GetLocation();
        Vector desiredLocation = destination - startPos;
        float size = sqrt(desiredLocation.X * desiredLocation.X + desiredLocation.Y * desiredLocation.Y); //2d vector size
        Vector desiredLocationNormalized = desiredLocation;
        desiredLocationNormalized.normalize();

        Vector centerLocation = startPos + (desiredLocationNormalized * ((size / 2.f)* (1500.f / speed)));
        Rotator desiredRotation = VectorToRotator(desiredLocation);

        Vector vec = { 0, 0, speed };
        Rotator rot = { desiredRotation.Pitch, desiredRotation.Yaw, desiredRotation.Roll };

        Vector aaa = RotateVectorWithQuat(vec, RotatorToQuat(rot));
        centerLocation = centerLocation + aaa;
        Vector directionToCenterUp = centerLocation - startPos;
        Vector directionToCenterUpNorm = directionToCenterUp;
        directionToCenterUpNorm.normalize();
        Vector finalCalc = directionToCenterUpNorm * (directionToCenterUp.magnitude() * 0.680f);

        return Vector(shotVelocity);
    }
    */

    //Avoid having random launches if user spams launch button
    if(launchIndex != launchNum) { return; }

    preparingToLaunch = false;

    ServerWrapper server = gameWrapper->GetGameEventAsServer();
    BallWrapper ball = server.GetBall();
    CarWrapper car = gameWrapper->GetLocalCar();

    //this is what needs to be calculated with prediction plugin code
    Vector targetLocation = car.GetLocation() + nextLaunch.spreadLocation;
    Vector launchDirection = targetLocation - ball.GetLocation();
    launchDirection.normalize();
    ball.SetVelocity(launchDirection * 5000 * nextLaunch.launchMagnitude + car.GetVelocity());

    /*int randOffsetScale = 50;
    float randX = rand() % randOffsetScale;
    float randY = rand() % randOffsetScale;
    Vector randOffset = {randX, randY, 0};
    Vector launchAngle = CalculateLaunchAngle(ball.GetLocation(), car.GetLocation() + randOffset, 5000 * nextLaunch.launchMagnitude);*/
}

Vector DribbleTrainer::CalculateLaunchAngle(Vector start, Vector target, float v)
{
    //Convert 3D trajectory into 2D equation, just solving for angle to reach X coordinate
    //https://en.wikipedia.org/wiki/Projectile_motion --> Angle theta required to hit coordinate (x,y)
    //y == car.Z - ball.Z (puts ball Z at 0)

    float y = -start.Z + 80;//target the top of the car
    target.Z = start.Z = 0;
    float x = (target - start).magnitude();

    float g = -9.8f;

    bool addIt = true;
    float launchAngle = sqrtf((v*v*v*v) - g * ((g*x*x) + (2*y*v*v)));
    float tanTheta = addIt ? v*v + launchAngle : v*v - launchAngle;
    tanTheta /= g*x;

    return atanf(tanTheta);
}
