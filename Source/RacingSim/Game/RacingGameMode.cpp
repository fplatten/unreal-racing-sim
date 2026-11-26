#include "Game/RacingGameMode.h"
#include "Car/RacingController.h"
#include "Car/RaceCar.h"
#include "Game/RaceDirector.h"
#include "Game/RacingHUD.h"
#include "Track/OvalTrack.h"
#include "Track/Scenery.h"
#include "Track/WallContactFilter.h"
#include "Car/TireSmoke.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/CollisionProfile.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogRacingSim, Log, All);

namespace
{
	constexpr float MphPerMs = 2.23694f;
	constexpr float ResultsDelay = 2.f;
	constexpr float RepeatDelay = 0.4f, RepeatInterval = 0.08f;
	constexpr float SplashFadeTime = 0.6f;
	const TCHAR* LoadSteps[] = {
		TEXT("Lighting the sky"), TEXT("Building the 2.5-mile oval"), TEXT("Raising the grandstands"),
		TEXT("Starting the race director"), TEXT("Rolling out the field"), TEXT("Tuning the engines"), TEXT("Warming up the tires") };
	constexpr int32 NumLoadSteps = UE_ARRAY_COUNT(LoadSteps);
	/** The loading step's text (the track and scenery steps name the track being built). */
	FString LoadStepText(int32 Step)
	{
		if (FTrackSpline::IsRoadCourse())
		{
			if (Step == 1) return TEXT("Building the 4.352-mile road course");
			if (Step == 2) return TEXT("Planting the forest");
			if (Step == 4) return TEXT("Lining up the grid");
		}
		return LoadSteps[Step];
	}

	/**
	 * Low warm golden-hour sun, sky, sky light and haze (stands in for the Java Environment class:
	 * sun 13 degrees up at azimuth 60 degrees, from Blender's frame).
	 */
	ASkyLight* SpawnEnvironment(UWorld* World)
	{
		constexpr float SunElevation = 13.f, SunAzimuth = 60.f;
		const FRotator SunRot(-SunElevation, 180.f - SunAzimuth, 0.f);
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector::ZeroVector, SunRot);
		UDirectionalLightComponent* L = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
		L->SetMobility(EComponentMobility::Movable);
		L->SetIntensity(7.f);
		L->SetLightColor(FLinearColor(1.0f, 0.82f, 0.60f));
		L->SetDynamicShadowDistanceMovableLight(40000.f);

		// the golden-hour sky painted in Blender (blender/make_sky.py) on a dome around everything;
		// its painted sun sits where the directional light comes from
		AStaticMeshActor* SkyDome = World->SpawnActor<AStaticMeshActor>();
		UStaticMeshComponent* Dome = SkyDome->GetStaticMeshComponent();
		// no collision at all, before it gets a mesh: a blocking 8 km sphere around the track
		// stops every car dead
		Dome->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		Dome->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Dome->SetGenerateOverlapEvents(false);
		SkyDome->SetActorEnableCollision(false);
		Dome->SetMobility(EComponentMobility::Movable);
		Dome->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
		UMaterialInterface* SkyMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Sky.M_Sky"));
		Dome->SetMaterial(0, SkyMat);
		Dome->SetWorldScale3D(FVector(16000.f));   // 8 km radius
		Dome->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Dome->SetCastShadow(false);

		ASkyLight* Sky = World->SpawnActor<ASkyLight>();
		USkyLightComponent* S = Sky->GetLightComponent();
		S->SetMobility(EComponentMobility::Movable);
		// captured once the scene is complete (see LoadingTick): the painted sky dome lights everything
		S->bRealTimeCapture = false;
		S->SourceType = ESkyLightSourceType::SLS_CapturedScene;
		// fill light from the sky: the low sun leaves the faces of the back-straight stands in shade
		S->SetIntensity(2.2f);
		S->SetLowerHemisphereColor(FLinearColor(0.18f, 0.20f, 0.14f));
		S->bLowerHemisphereIsBlack = false;
		S->RecaptureSky();
		AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>();
		Fog->GetComponent()->SetFogDensity(0.0015f);
		Fog->GetComponent()->SetFogInscatteringColor(FLinearColor(0.55f, 0.50f, 0.45f));

		// a fixed exposure and a warm, slightly contrasty grade (the Java ColorGradeFilter)
		APostProcessVolume* Pp = World->SpawnActor<APostProcessVolume>();
		Pp->bUnbound = true;
		FPostProcessSettings& P = Pp->Settings;
		P.bOverride_AutoExposureBias = true;
		P.AutoExposureBias = -0.6f;
		P.bOverride_ColorSaturation = true;
		P.ColorSaturation = FVector4(1.08, 1.04, 0.96, 1.0);
		P.bOverride_ColorContrast = true;
		P.ColorContrast = FVector4(1.08, 1.08, 1.08, 1.0);
		P.bOverride_WhiteTemp = true;
		P.WhiteTemp = 5900.f;
		P.bOverride_VignetteIntensity = true;
		P.VignetteIntensity = 0.25f;
		P.bOverride_MotionBlurAmount = true;
		P.MotionBlurAmount = 0.f;
		return Sky;
	}
	// ---- liveries (port of com.racingsim.vehicle.Livery; colors are sRGB) ----
	struct FLivery
	{
		FString Name;
		FLinearColor Primary, Secondary, Accent;
		int32 Number;
	};

	FLinearColor Srgb(float R, float G, float B)
	{
		return FLinearColor::FromSRGBColor(FColor(FMath::RoundToInt(R * 255), FMath::RoundToInt(G * 255), FMath::RoundToInt(B * 255)));
	}

	FLinearColor Hsv(float H, float S, float V)
	{
		float R, G, B;
		const int32 I = (int32)(H * 6) % 6;
		const float F = H * 6 - (int32)(H * 6), P = V * (1 - S), Q = V * (1 - F * S), T = V * (1 - (1 - F) * S);
		switch (I)
		{
		case 0: R = V; G = T; B = P; break;
		case 1: R = Q; G = V; B = P; break;
		case 2: R = P; G = V; B = T; break;
		case 3: R = P; G = Q; B = V; break;
		case 4: R = T; G = P; B = V; break;
		default: R = V; G = P; B = Q; break;
		}
		return Srgb(R, G, B);
	}

	FLivery LiveryForIndex(int32 Index)
	{
		static const FLivery Palette[] = {
			{ TEXT("Player"), Srgb(0.05f, 0.22f, 0.72f), Srgb(0.96f, 0.96f, 0.97f), Srgb(0.86f, 0.08f, 0.10f), 5 },
			{ TEXT("Crimson"), Srgb(0.78f, 0.03f, 0.05f), Srgb(0.97f, 0.97f, 0.97f), Srgb(0.08f, 0.08f, 0.09f), 12 },
			{ TEXT("Papaya"), Srgb(1.00f, 0.45f, 0.03f), Srgb(0.07f, 0.17f, 0.45f), Srgb(0.96f, 0.96f, 0.96f), 7 },
			{ TEXT("Viper"), Srgb(0.04f, 0.45f, 0.16f), Srgb(0.96f, 0.96f, 0.96f), Srgb(0.98f, 0.80f, 0.08f), 9 },
			{ TEXT("Midnight"), Srgb(0.03f, 0.03f, 0.04f), Srgb(0.10f, 0.10f, 0.12f), Srgb(0.90f, 0.70f, 0.18f), 3 },
			{ TEXT("Sunburst"), Srgb(0.98f, 0.78f, 0.04f), Srgb(0.98f, 0.84f, 0.10f), Srgb(0.08f, 0.08f, 0.10f), 22 },
			{ TEXT("Magenta"), Srgb(0.80f, 0.05f, 0.50f), Srgb(0.97f, 0.97f, 0.97f), Srgb(0.10f, 0.10f, 0.12f), 14 },
			{ TEXT("Teal"), Srgb(0.00f, 0.48f, 0.54f), Srgb(0.96f, 0.96f, 0.96f), Srgb(1.00f, 0.50f, 0.05f), 31 },
		};
		static const TCHAR* MoreNames[] = {
			TEXT("Cobalt"), TEXT("Lime"), TEXT("Scarlet"), TEXT("Indigo"), TEXT("Amber"), TEXT("Jade"), TEXT("Ruby"), TEXT("Onyx"),
			TEXT("Saffron"), TEXT("Azure"), TEXT("Coral"), TEXT("Emerald"), TEXT("Copper"), TEXT("Violet"), TEXT("Silver"),
			TEXT("Tangerine"), TEXT("Navy"), TEXT("Mint"), TEXT("Garnet"), TEXT("Bronze"), TEXT("Sky"), TEXT("Plum"), TEXT("Gold"),
			TEXT("Forest"), TEXT("Rose") };
		static const int32 MoreNumbers[] = { 2, 4, 6, 8, 10, 11, 15, 18, 20, 21, 23, 24, 26, 27, 28, 29, 30, 33, 45, 48, 51, 60, 66, 77, 98 };
		if (Index < UE_ARRAY_COUNT(Palette)) return Palette[Index];
		const int32 K = Index - UE_ARRAY_COUNT(Palette);
		const float Hue = FMath::Fmod(K * 0.618034f + 0.11f, 1.f);   // golden-ratio hue spacing
		FLivery L;
		L.Primary = Hsv(Hue, 0.85f, K % 3 == 2 ? 0.55f : 0.85f);
		L.Secondary = K % 3 == 0 ? Srgb(0.96f, 0.96f, 0.96f) : K % 3 == 1 ? Srgb(0.07f, 0.07f, 0.09f) : Hsv(FMath::Fmod(Hue + 0.5f, 1.f), 0.25f, 0.95f);
		L.Accent = Hsv(FMath::Fmod(Hue + 0.45f, 1.f), 0.9f, 0.95f);
		L.Name = MoreNames[K % UE_ARRAY_COUNT(MoreNames)];
		L.Number = MoreNumbers[K % UE_ARRAY_COUNT(MoreNumbers)];
		return L;
	}

	FString LapStr(float T)
	{
		return FMath::IsNaN(T) ? FString(TEXT("--.--")) : FString::Printf(TEXT("%.2f"), T);
	}

	bool Pressed(APlayerController* Pc, std::initializer_list<FKey> Keys)
	{
		for (const FKey& K : Keys) if (Pc->WasInputKeyJustPressed(K)) return true;
		return false;
	}

	bool Down(APlayerController* Pc, std::initializer_list<FKey> Keys)
	{
		for (const FKey& K : Keys) if (Pc->IsInputKeyDown(K)) return true;
		return false;
	}
}

bool FMenuSettings::Change(int32 Delta)
{
	switch (Row)
	{
	case TrackRow:
		Track = Track == ETrackLayout::Oval ? ETrackLayout::Road : ETrackLayout::Oval;
		Downforce = DefaultDownforce(Track);
		CustomLaps = -1;
		if (!HasPits()) PitStops = 0;
		break;
	case 1:
		CustomLaps = -1;
		LapIndex = (LapIndex + Delta + 2) % 2;
		break;
	case 2:
		Opponents = FMath::Clamp(Opponents + Delta, MinOpponents, MaxOpponents);
		StartPosition = FMath::Min(StartPosition, FieldSize());
		break;
	case 3:
		StartPosition = FMath::Clamp(StartPosition + Delta, 1, FieldSize());
		break;
	case PitRow:
		PitStops = HasPits() ? FMath::Clamp(PitStops + Delta, 0, MaxPitStops) : 0;
		break;
	default:
		Downforce = FMath::Clamp(Downforce + Delta, 0, DownforceSteps);
		break;
	}
	return Row == 2 || Row == 3;
}

ARacingGameMode::ARacingGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bTickEvenWhenPaused = true;
	DefaultPawnClass = nullptr;
	HUDClass = ARacingHUD::StaticClass();
}

APlayerController* ARacingGameMode::Pc() const
{
	return UGameplayStatics::GetPlayerController(GetWorld(), 0);
}

void ARacingGameMode::StartPlay()
{
	const TCHAR* Cmd = FCommandLine::Get();
	bSimCheck = FParse::Param(Cmd, TEXT("SimCheck"));
	bWallCheck = FParse::Param(Cmd, TEXT("WallCheck"));
	bCarCheck = FParse::Param(Cmd, TEXT("CarCheck"));
	if (bCarCheck)
	{
		bWallCheck = true;   // the same set-up: one car alone, scripted (CarCheckTick takes over below)
		FTrackSpline::SelectLayout(ETrackLayout::Oval);
	}
	if (bWallCheck)
	{
		// one car, alone on the track
		bSimCheck = false;
		Menu.Opponents = 0;
		Menu.StartPosition = 1;
	}
	// the track: ?Track= (the menu reloads the level with it and the other settings), else -Track=
	{
		FString TrackKey;
		FParse::Value(Cmd, TEXT("Track="), TrackKey);
		if (UGameplayStatics::HasOption(OptionsString, TEXT("Track"))) TrackKey = UGameplayStatics::ParseOption(OptionsString, TEXT("Track"));
		Menu.Track = FTrackSpline::LayoutFromKey(TrackKey);
		FTrackSpline::SelectLayout(Menu.Track);
		Menu.Downforce = FMenuSettings::DefaultDownforce(Menu.Track);
	}
	bAutopilot = FParse::Param(Cmd, TEXT("Autopilot"));
	bQuickStart = FParse::Param(Cmd, TEXT("QuickStart"));
	bShotMenu = FParse::Param(Cmd, TEXT("ShotMenu"));
	int32 Laps = -1;
	FParse::Value(Cmd, TEXT("Opponents="), Menu.Opponents);
	FParse::Value(Cmd, TEXT("Start="), Menu.StartPosition);
	FParse::Value(Cmd, TEXT("Pits="), Menu.PitStops);
	FParse::Value(Cmd, TEXT("Downforce="), Menu.Downforce);
	if (FParse::Value(Cmd, TEXT("Laps="), Laps) && Laps > 0) Menu.CustomLaps = Laps;
	// settings carried over a change of track
	if (UGameplayStatics::HasOption(OptionsString, TEXT("Track")))
	{
		Menu.Opponents = UGameplayStatics::GetIntOption(OptionsString, TEXT("Opponents"), Menu.Opponents);
		Menu.StartPosition = UGameplayStatics::GetIntOption(OptionsString, TEXT("Start"), Menu.StartPosition);
		Menu.LapIndex = FMath::Clamp(UGameplayStatics::GetIntOption(OptionsString, TEXT("LapIndex"), Menu.LapIndex), 0, 1);
		Menu.Downforce = UGameplayStatics::GetIntOption(OptionsString, TEXT("Downforce"), Menu.Downforce);
		Menu.Row = FMenuSettings::TrackRow;
	}
	if (!Menu.HasPits()) Menu.PitStops = 0;
	FParse::Value(Cmd, TEXT("Cars="), SimCars);
	FParse::Value(Cmd, TEXT("Seconds="), CheckSeconds);
	Menu.Opponents = FMath::Clamp(Menu.Opponents, bWallCheck ? 0 : FMenuSettings::MinOpponents, FMenuSettings::MaxOpponents);
	Menu.StartPosition = FMath::Clamp(Menu.StartPosition, 1, Menu.FieldSize());
	Menu.PitStops = FMath::Clamp(Menu.PitStops, 0, FMenuSettings::MaxPitStops);
	Menu.Downforce = FMath::Clamp(Menu.Downforce, 0, FMenuSettings::DownforceSteps);
	FString Shots;
	if (FParse::Value(Cmd, TEXT("AutoShot="), Shots, /*bShouldStopOnSeparator*/ false))
	{
		TArray<FString> Parts;
		Shots.ParseIntoArray(Parts, TEXT(","));
		for (const FString& P : Parts) ShotTimes.Add(FCString::Atof(*P));
		if (!bShotMenu) bQuickStart = true;
	}
	FString Cam;
	FParse::Value(Cmd, TEXT("ShotCam="), Cam);
	bShotTCam = Cam == TEXT("tcam");
	bShotShowcase = Cam == TEXT("showcase");
	bShotNoHud = FParse::Param(Cmd, TEXT("ShotNoHud"));

	Super::StartPlay();
	SplashStatus = LoadStepText(0) + TEXT("...");
	if (APlayerController* P = Pc())
	{
		P->SetInputMode(FInputModeGameOnly());
		P->bShowMouseCursor = false;
	}
}

// ------------------------------------------------------------------------------- loading

void ARacingGameMode::LoadingTick(float Dt)
{
	// one step per frame so the splash's loading bar moves (as in RaceApp.updateLoading)
	UWorld* World = GetWorld();
	switch (LoadStep)
	{
	case 0:
		SkyLight = SpawnEnvironment(World);
		break;
	case 1:
		Track = AOvalTrack::Get(World);
		if (!Track) Track = World->SpawnActor<AOvalTrack>();
		break;
	case 2:
		if (!bSimCheck)
		{
			Scenery = World->SpawnActor<AScenery>();
			Scenery->Build(Track);
		}
		break;
	case 3:
		Director = World->SpawnActor<ARaceDirector>();
		Director->GetRacingLine();
		MenuCam = World->SpawnActor<ACameraActor>();
		MenuCam->GetCameraComponent()->SetFieldOfView(72.f);
		break;
	case 4:
		if (bSimCheck) BuildSimCheckField(SimCars, Menu.Downforce);
		else BuildField();
		if (bWallCheck)
		{
			Watched->SetScriptedThrottle(0.f);
			Watched->SetDrivingEnabled(false);
		}
		Smoke = World->SpawnActor<ATireSmoke>();
		Smoke->SetCars(FieldCars);
		break;
	case 5:
		if (!bSimCheck) Audio.Init(this);
		break;
	default:
		// a few frames to compile shaders and stream textures behind the splash
		if (++WarmupFrames == 20 && SkyLight) SkyLight->GetLightComponent()->RecaptureSky();
		if (WarmupFrames < 45) break;
		SplashTarget = 1.f;
		SplashStatus = TEXT("Ready");
		if (SplashShown < 0.999f) break;
		SplashFade = SplashFadeTime;
		if (bWallCheck)
		{
			State = ERaceState::Race;
			StateStartTime = GetWorld()->GetRealTimeSeconds();
			if (Pc()) Pc()->SetViewTarget(Watched);
			return;
		}
		if (bSimCheck || bQuickStart)
		{
			EnterMenu();
			StartRace();
		}
		else
		{
			EnterMenu();
		}
		return;
	}
	if (LoadStep < NumLoadSteps)
	{
		LoadStep++;
		SplashTarget = LoadStep / (float)(NumLoadSteps + 1);
		SplashStatus = LoadStepText(FMath::Min(LoadStep, NumLoadSteps - 1)) + TEXT("...");
	}
}

float ARacingGameMode::GetSplashOpacity() const
{
	if (State == ERaceState::Loading) return 1.f;
	return FMath::Clamp(SplashFade / SplashFadeTime, 0.f, 1.f);
}

// --------------------------------------------------------------------------------- field

float ARacingGameMode::AiGripBase()
{
	// the lowest grip the AI plans on: the Indy car's tires give 1.55, the Formula car's 1.9
	return FVehicleSpec::ForTrack().Type == ECarType::Formula ? 1.50f : 1.22f;
}

void ARacingGameMode::AiParams(int32 Index, float& Grip, float& Aggression)
{
	while (AiParamPool.Num() <= Index)
	{
		const float G = AiGripBase() + AiRandom.NextFloat() * 0.10f;   // planned grip
		const float A = AiRandom.NextFloat();   // driver type: 0 rookie .. 1 veteran
		AiParamPool.Add(FVector2f(G, A));
	}
	Grip = AiParamPool[Index].X;
	Aggression = AiParamPool[Index].Y;
}

ARaceCar* ARacingGameMode::SpawnCar(int32 Slot, int32 CarCount, int32 LiveryIndex, bool bHuman, const FString& NameOverride)
{
	float S, D;
	ARaceDirector::GridSlot(Slot, CarCount, S, D);
	const FTrackSpline& Spline = Track->GetSpline();
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Pos = Spline.Point(S, D) * 100.0 + FVector(0, 0, 60);
	ARaceCar* Car = GetWorld()->SpawnActor<ARaceCar>(ARaceCar::StaticClass(), Pos, Spline.Forward(S).Rotation(), Params);
	const FLivery L = LiveryForIndex(LiveryIndex);
	Car->SetIdentity(NameOverride.IsEmpty() ? L.Name : NameOverride, L.Number, bHuman, L.Primary, L.Secondary, L.Accent);
	FieldCars.Add(Car);
	return Car;
}

void ARacingGameMode::BuildField()
{
	// you plus the opponents in grid order; opponent i always has the same livery, grip and
	// aggression (as RaceApp's pool of AI cars)
	Director->ClearField();
	if (APlayerController* P = Pc()) P->UnPossess();
	for (ARaceCar* Car : FieldCars) if (Car) Car->Destroy();
	FieldCars.Reset();

	const FRacingLine* Line = Director->GetRacingLine();
	auto MakeDriver = [Line](ARaceCar* Car, float Grip, float Aggression)
	{
		FCarCore& C = Car->Core();
		const FSpeedProfile Profile(Line, C.Spec(), C.Sim->GetAero(), Grip, 13.f);
		return MakeUnique<FAIDriver>(&C, Line, Profile, Aggression);
	};
	const int32 CarCount = Menu.FieldSize();
	const int32 PlayerSlot = FMath::Clamp(Menu.StartPosition - 1, 0, Menu.Opponents);
	int32 NextAi = 0;
	for (int32 Slot = 0; Slot < CarCount; Slot++)
	{
		if (Slot == PlayerSlot)
		{
			ARaceCar* Car = SpawnCar(Slot, CarCount, 0, true, TEXT("YOU"));
			Director->AddCar(Car, bAutopilot ? MakeDriver(Car, AiGripBase() + 0.08f, 0.6f) : nullptr);
			Watched = Car;
			continue;
		}
		float Grip, Aggression;
		AiParams(NextAi, Grip, Aggression);
		ARaceCar* Car = SpawnCar(Slot, CarCount, ++NextAi, false, FString());
		Director->AddCar(Car, MakeDriver(Car, Grip, Aggression));
	}
	Director->FinishField();
	if (Smoke) Smoke->SetCars(FieldCars);
	if (APlayerController* P = Pc()) P->Possess(Watched);
	Watched->SetDrivingEnabled(State == ERaceState::Race);
	if (State != ERaceState::Race && MenuCam && Pc()) Pc()->SetViewTarget(MenuCam);
}

void ARacingGameMode::BuildSimCheckField(int32 CarCount, int32 Downforce)
{
	// the same field as HeadlessSimCheck with -Pplayer=ai: every car AI driven, liveries in
	// slot order, grip and aggression drawn from Random(42) in slot order
	CarCount = FMath::Clamp(CarCount, 2, 33);
	const FRacingLine* Line = Director->GetRacingLine();
	FJavaRandom Rnd(42);
	FAIDriver::Stats = FAIDriver::FStats();
	// tuning: -AiGrip=<lowest planned grip> -AiGripSpread=<range above it>
	float GripBase = AiGripBase(), GripSpread = 0.10f;
	FParse::Value(FCommandLine::Get(), TEXT("AiGrip="), GripBase);
	FParse::Value(FCommandLine::Get(), TEXT("AiGripSpread="), GripSpread);
	const int32 WatchedSlot = FMath::Min(5, CarCount - 1);
	for (int32 Slot = 0; Slot < CarCount; Slot++)
	{
		ARaceCar* Car = SpawnCar(Slot, CarCount, Slot, false, FString());
		if (Slot == WatchedSlot) Car->Core().Sim->GetAero().SetDownforceLevel(Downforce / 10.f);
		const float Grip = GripBase + Rnd.NextFloat() * GripSpread;
		const float Aggression = Rnd.NextFloat();   // driver type: 0 rookie .. 1 veteran
		FCarCore& C = Car->Core();
		Director->AddCar(Car, MakeUnique<FAIDriver>(&C, Line, FSpeedProfile(Line, C.Spec(), C.Sim->GetAero(), Grip, 13.f), Aggression));
		if (Slot == WatchedSlot) Watched = Car;
	}
	Director->FinishField();
	if (APlayerController* P = Pc()) P->Possess(Watched);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: %d cars, %d laps, %d stops, watching %s"), CarCount, Menu.Laps(), Menu.PitStops, *Watched->Core().Name);
}

// ---------------------------------------------------------------------------- state changes

void ARacingGameMode::EnterMenu()
{
	State = ERaceState::Menu;
	StateStartTime = GetWorld()->GetRealTimeSeconds();
	Director->RequestStop();
	if (Watched) Watched->SetDrivingEnabled(false);
	if (MenuCam && Pc()) Pc()->SetViewTarget(MenuCam);
}

void ARacingGameMode::StartRace()
{
	State = ERaceState::Race;
	StateStartTime = GetWorld()->GetRealTimeSeconds();
	if (!bSimCheck) Director->SetPlayerDownforce(Menu.Downforce / (float)FMenuSettings::DownforceSteps);
	Director->RequestStart(Menu.Laps(), Menu.PitStops);
	FinishedFor = 0.f;
	if (Smoke) Smoke->Clear();
	if (Watched)
	{
		Watched->SetDrivingEnabled(true);
		if (bShotTCam) Watched->SetTCam(true);
		if (bShotShowcase) Watched->SetShowcase(true);
		// clean renders (the splash art): no HUD drawn over the scene
		if (bShotNoHud && Pc() && Pc()->GetHUD()) Pc()->GetHUD()->bShowHUD = false;
		if (Pc()) Pc()->SetViewTarget(Watched);
	}
}

void ARacingGameMode::ShowResults()
{
	State = ERaceState::Results;
}

void ARacingGameMode::OpenConfirm()
{
	bConfirmOpen = true;
	// pause: no physics steps means the race clock, cars and AI all stand still
	if (State == ERaceState::Race) UGameplayStatics::SetGamePaused(GetWorld(), true);
}

void ARacingGameMode::CloseConfirm(bool bResume)
{
	bConfirmOpen = false;
	if (bResume) UGameplayStatics::SetGamePaused(GetWorld(), false);
}

void ARacingGameMode::AcceptConfirm()
{
	if (State == ERaceState::Race || State == ERaceState::Results)
	{
		CloseConfirm(true);
		EnterMenu();
	}
	else
	{
		FGenericPlatformMisc::RequestExit(false);
	}
}

void ARacingGameMode::GetConfirmText(FString& Title, FString& Message, FString& EnterText, FString& EscText) const
{
	if (State == ERaceState::Race)
	{
		Title = TEXT("LEAVE THE RACE?");
		Message = TEXT("The race will be abandoned and you go back to the menu.");
		EnterText = TEXT("quit the race and return to the menu");
		EscText = TEXT("return to the race");
	}
	else
	{
		Title = TEXT("QUIT THE GAME?");
		Message = TEXT("Thanks for racing!");
		EnterText = TEXT("quit");
		EscText = TEXT("return to the game");
	}
}

// --------------------------------------------------------------------------------- input

void ARacingGameMode::ChangeSetting(int32 Dir)
{
	if (bReloading) return;
	const bool bTrack = Menu.Row == FMenuSettings::TrackRow;
	if (Menu.Change(Dir)) BuildField();
	if (bTrack) ReloadForTrack();
}

void ARacingGameMode::ReloadForTrack()
{
	// a different track is a different world: load the level again (the splash shows while the
	// new track is built), carrying the menu over
	bReloading = true;
	HeldDirection = 0;
	const FString Options = FString::Printf(TEXT("Track=%s?Opponents=%d?Start=%d?LapIndex=%d?Downforce=%d"),
		FTrackSpline::LayoutKey(Menu.Track), Menu.Opponents, Menu.StartPosition, Menu.LapIndex, Menu.Downforce);
	UE_LOG(LogRacingSim, Display, TEXT("Switching track: %s"), *Options);
	UGameplayStatics::OpenLevel(GetWorld(), FName(*UGameplayStatics::GetCurrentLevelName(GetWorld(), true)), true, Options);
}

void ARacingGameMode::ToggleQuality()
{
	// high quality post effects (ambient occlusion, bloom, light shafts) on / off
	bHighQuality = !bHighQuality;
	if (APlayerController* P = Pc())
	{
		P->ConsoleCommand(bHighQuality ? TEXT("sg.PostProcessQuality 3") : TEXT("sg.PostProcessQuality 1"));
		P->ConsoleCommand(bHighQuality ? TEXT("sg.ShadowQuality 3") : TEXT("sg.ShadowQuality 1"));
	}
}

void ARacingGameMode::HandleInput(float Dt)
{
	APlayerController* P = Pc();
	if (!P || State == ERaceState::Loading || SplashFade > 0.f) return;
	// keyboard, gamepad and racing wheel: A / OK = Enter, Start = Esc, B backs out of prompts and
	// the results (never pauses a race by accident), the d-pad works the menu
	using EWheel = FRacingController::EButton;
	const FRacingController& W = FRacingController::Get();
	const bool bEnter = Pressed(P, { EKeys::Enter, EKeys::Gamepad_FaceButton_Bottom }) || W.WasPressed(EWheel::Ok);
	const bool bBack = P->WasInputKeyJustPressed(EKeys::Gamepad_FaceButton_Right) || W.WasPressed(EWheel::Back);
	const bool bEsc = Pressed(P, { EKeys::Escape, EKeys::Gamepad_Special_Right }) || W.WasPressed(EWheel::Pause)
		|| (bBack && (bConfirmOpen || State != ERaceState::Race));
	if (P->WasInputKeyJustPressed(EKeys::H)) bShowHelp = !bShowHelp;
	if (P->WasInputKeyJustPressed(EKeys::G)) ToggleQuality();

	if (bConfirmOpen)   // modal: only Enter / Esc
	{
		if (bEnter) AcceptConfirm();
		else if (bEsc) CloseConfirm(true);
		return;
	}
	if (bEsc)
	{
		HeldDirection = 0;
		if (State == ERaceState::Results) EnterMenu();   // the race is over: straight back to the menu
		else OpenConfirm();
		return;
	}
	if (State == ERaceState::Menu)
	{
		if (Pressed(P, { EKeys::Up, EKeys::Gamepad_DPad_Up }) || W.WasPressed(EWheel::Up)) Menu.Row = (Menu.Row + FMenuSettings::Rows - 1) % FMenuSettings::Rows;
		if (Pressed(P, { EKeys::Down, EKeys::Gamepad_DPad_Down }) || W.WasPressed(EWheel::Down)) Menu.Row = (Menu.Row + 1) % FMenuSettings::Rows;
		const bool bLeft = Pressed(P, { EKeys::Left, EKeys::Gamepad_DPad_Left }) || W.WasPressed(EWheel::Left);
		const bool bRight = Pressed(P, { EKeys::Right, EKeys::Gamepad_DPad_Right }) || W.WasPressed(EWheel::Right);
		if (bLeft || bRight)
		{
			HeldDirection = bLeft ? -1 : 1;
			HeldTime = RepeatTimer = 0.f;
			ChangeSetting(HeldDirection);
		}
		else if (HeldDirection != 0)
		{
			// hold Left / Right to repeat
			const bool bHeld = HeldDirection < 0 ? Down(P, { EKeys::Left, EKeys::Gamepad_DPad_Left }) || W.IsDown(EWheel::Left)
				: Down(P, { EKeys::Right, EKeys::Gamepad_DPad_Right }) || W.IsDown(EWheel::Right);
			if (!bHeld)
			{
				HeldDirection = 0;
			}
			else
			{
				HeldTime += FMath::Min(Dt, 0.1f);
				if (HeldTime >= RepeatDelay)
				{
					RepeatTimer += FMath::Min(Dt, 0.1f);
					while (RepeatTimer >= RepeatInterval)
					{
						RepeatTimer -= RepeatInterval;
						ChangeSetting(HeldDirection);
					}
				}
			}
		}
		if (bEnter) StartRace();
	}
	else if (State == ERaceState::Results)
	{
		if (bEnter) EnterMenu();
	}
}

void ARacingGameMode::OrbitGrid(float Dt)
{
	// slow camera sweep along the field lined up on the grid, from the infield side
	if (!MenuCam || !Track) return;
	OrbitAngle += Dt * 0.15f;
	const int32 Cars = FieldCars.Num();
	float MidS, Dummy;
	ARaceDirector::GridSlot(Cars / 2, FMath::Max(Cars, 1), MidS, Dummy);
	const float Sweep = FMath::Sin(OrbitAngle);
	const FTrackSpline& Spline = Track->GetSpline();
	// (the oval: from the infield; a road course: from the run-off on the left)
	const float EyeD = FTrackSpline::IsOval() ? -13.f : FMath::Max(Spline.MinD(MidS) + 2.f, -11.f);
	const FVector Eye = Spline.Point(MidS + 32.f * Sweep, EyeD + 3.f * FMath::Cos(OrbitAngle * 0.7f)) + FVector(0, 0, 4.5f);
	const FVector Center = Spline.Point(MidS + 8.f * Sweep, 0.5f) + FVector(0, 0, 0.4f);
	MenuCam->SetActorLocationAndRotation(Eye * 100.0, (Center - Eye).Rotation());
}

// ---------------------------------------------------------------------------------- tick

void ARacingGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(DeltaSeconds, 0.1f);
	// racing wheel and pedals (DirectInput), read once a frame for the menus and the car
	if (!bSimCheck && !bWallCheck)
	{
		FRacingController& Wheel = FRacingController::Get();
		Wheel.Poll(Dt);
		// force feedback follows the car you are driving; light centering in the menus
		const ARaceCar* Mine = State == ERaceState::Race ? GetPlayerCar() : nullptr;
		Wheel.SetForceFeedback(Mine ? Mine->GetSnapshot().Speed : 0.f);
	}
	SplashShown += (SplashTarget - SplashShown) * FMath::Min(1.f, Dt * 12.f);
	if (SplashTarget - SplashShown < 0.002f) SplashShown = SplashTarget;
	if (State == ERaceState::Loading)
	{
		LoadingTick(Dt);
		return;
	}
	if (SplashFade > 0.f) SplashFade -= Dt;

	if (bCarCheck)
	{
		CarCheckTick();
		return;
	}
	if (bWallCheck)
	{
		WallCheckTick();
		return;
	}
	HandleInput(Dt);
	AutomationTick();
	if (APlayerController* P = Pc())
	{
		// race sound while racing and on the results screen; silent in the menu and behind a prompt
		const bool bSound = (State == ERaceState::Race || State == ERaceState::Results) && !bConfirmOpen;
		FVector CamPos = FVector::ZeroVector, CamLeft = FVector::LeftVector;
		if (P->PlayerCameraManager)
		{
			CamPos = P->PlayerCameraManager->GetCameraLocation() / 100.0;
			CamLeft = -P->PlayerCameraManager->GetCameraRotation().Quaternion().GetRightVector();
		}
		Audio.Update(Dt, Watched, FieldCars, CamPos, CamLeft, bSound);
		static const bool bAudioDebug = FParse::Param(FCommandLine::Get(), TEXT("AudioDebug"));
		static double NextAudioLog = 0.0;
		if (bAudioDebug && GetWorld()->GetRealTimeSeconds() >= NextAudioLog)
		{
			NextAudioLog = GetWorld()->GetRealTimeSeconds() + 1.0;
			UE_LOG(LogRacingSim, Display, TEXT("AUDIO state %d sound %d | %s"), (int32)State, bSound ? 1 : 0, *Audio.DebugState());
		}
	}
	if (bConfirmOpen) return;   // paused behind the prompt

	if (State == ERaceState::Menu)
	{
		OrbitGrid(Dt);
		return;
	}
	if (State == ERaceState::Race && Director && Watched)
	{
		const FRaceSnapshot R = Director->GetSnapshot();
		const FRaceSnapshot::FRow* Me = R.Standings.FindByPredicate([this](const FRaceSnapshot::FRow& Row) { return Row.Name == Watched->Core().Name; });
		if (Me && Me->bFinished && !bSimCheck)
		{
			FinishedFor += Dt;
			if (FinishedFor > ResultsDelay) ShowResults();
		}
	}
}

// ------------------------------------------------------------------- automated checks

void ARacingGameMode::AutomationTick()
{
	if (!bSimCheck && ShotTimes.Num() == 0) return;
	const double RealT = GetWorld()->GetRealTimeSeconds() - StateStartTime;
	if (bShotMenu && State == ERaceState::Menu && NextShot < ShotTimes.Num() && RealT >= ShotTimes[NextShot])
	{
		const FString File = FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("autoshot_%02d.png"), NextShot);
		FScreenshotRequest::RequestScreenshot(File, !bShotNoHud, false);
		NextShot++;
		return;
	}
	if (bShotMenu && NextShot >= ShotTimes.Num() && RealT >= ShotTimes.Last() + 1.0)
	{
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	if ((State != ERaceState::Race && State != ERaceState::Results) || !Watched || !Director) return;

	const FCarSnapshot C = Watched->GetSnapshot();
	const FRaceSnapshot R = Director->GetSnapshot();
	if (C.SimTime <= 0.0 || !R.bRunning) return;
	if (StartSimTime < 0.0) StartSimTime = C.SimTime;
	const double T = C.SimTime - StartSimTime;
	WatchedTop = FMath::Max(WatchedTop, C.Speed);
	// the gaps at the front (SimCheck)
	if (bSimCheck && R.Standings.Num() >= 3 && T >= NextFrontLog)
	{
		NextFrontLog = T + (R.RaceTime < 100.f ? 5.0 : 15.0);
		const TArray<FRaceSnapshot::FRow>& St = R.Standings;
		UE_LOG(LogRacingSim, Display, TEXT("SimCheck: front t=%.0f  P1 %s  %.0f m  P2 %s  %.0f m  P3 %s"), T,
			*St[0].Name, St[0].TotalDistance - St[1].TotalDistance, *St[1].Name, St[1].TotalDistance - St[2].TotalDistance, *St[2].Name);
	}
	// racing at the front: lead changes and changes in the top 3, once the start has settled
	if (bSimCheck && R.RaceTime > 100.f && R.Standings.Num() >= 3)
	{
		// a change counts once the new order has held for 3 s (cars side by side swap back and forth)
		TArray<FString> Top3;
		for (int32 i = 0; i < 3; i++) Top3.Add(R.Standings[i].bFinished ? FString() : R.Standings[i].Name);
		if (!Top3.Contains(FString()))
		{
			if (Top3 != PendingTop3)
			{
				PendingTop3 = Top3;
				PendingSince = T;
			}
			else if (T - PendingSince >= 3.0 && Top3 != LastTop3)
			{
				if (LastTop3.Num() == 3)
				{
					if (Top3[0] != LastTop3[0]) LeadChanges++;
					Top3Changes++;
				}
				LastTop3 = Top3;
			}
		}
	}

	if (!bShotMenu && NextShot < ShotTimes.Num() && T >= ShotTimes[NextShot])
	{
		const FString File = FPaths::ProjectSavedDir() / TEXT("Screenshots") / FString::Printf(TEXT("autoshot_%02d.png"), NextShot);
		FScreenshotRequest::RequestScreenshot(File, !bShotNoHud, false);
		UE_LOG(LogRacingSim, Display, TEXT("SimCheck: screenshot %s"), *File);
		NextShot++;
	}
	if (!bSimCheck)
	{
		if (!bShotMenu && NextShot >= ShotTimes.Num() && T >= ShotTimes.Last() + 1.0) FGenericPlatformMisc::RequestExit(false);
		return;
	}

	const FRaceSnapshot::FRow* Me = R.Standings.FindByPredicate([this](const FRaceSnapshot::FRow& Row) { return Row.Name == Watched->Core().Name; });
	if (T >= NextLogTime && Me)
	{
		NextLogTime += T < 6.0 ? 0.5 : 5.0;
		UE_LOG(LogRacingSim, Display, TEXT("SimCheck: t=%6.1f s=%7.1f d=%5.2f v=%6.1fmph P%d lap %d/%d race=%.1f rec=%d contacts=%d  thr %.2f brk %.2f gear %d wheels %d comZ %.2f upZ %.2f running %d"),
			T, C.TrackS, C.TrackD, C.Speed * MphPerMs, Me->Position, Me->Lap, R.RaceLaps, R.RaceTime, R.Recoveries, ARaceCar::CarContacts.load(),
			C.Throttle, C.Brake, C.Gear, C.Contacts, C.ComPos.Z, C.Up.Z, R.bRunning ? 1 : 0);
	}
	const bool bAllFinished = R.Standings.Num() > 0 && !R.Standings.ContainsByPredicate([](const FRaceSnapshot::FRow& Row) { return !Row.bFinished; });
	if (bAllFinished && AllFinishedAt < 0.0) AllFinishedAt = T;
	if ((AllFinishedAt >= 0.0 && T > AllFinishedAt + 1.0) || T > CheckSeconds)
	{
		LogClassification();
		bSimCheck = false;
		FGenericPlatformMisc::RequestExit(false);
	}
}

void ARacingGameMode::LogClassification()
{
	const FRaceSnapshot R = Director->GetSnapshot();
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: car-car contacts %d, recoveries %d, max %s speed %.1f mph"),
		ARaceCar::CarContacts.load(), R.Recoveries, *Watched->Core().Name, WatchedTop * MphPerMs);
	float BestSum = 0.f, Fastest = TNumericLimits<float>::Max();
	int32 BestCount = 0;
	for (const FRaceSnapshot::FRow& Row : R.Standings)
	{
		UE_LOG(LogRacingSim, Display, TEXT("SimCheck:   P%d %-9s laps %d pits %d %s best %s"), Row.Position, *Row.Name, Row.LapsCompleted,
			Row.PitStops, Row.bFinished ? *FString::Printf(TEXT("FINISHED %.2fs"), Row.FinishTime) : TEXT("running"), *LapStr(Row.BestLap));
		if (!FMath::IsNaN(Row.BestLap))
		{
			BestSum += Row.BestLap;
			BestCount++;
			Fastest = FMath::Min(Fastest, Row.BestLap);
		}
	}
	const FAIDriver::FStats& A = FAIDriver::Stats;
	const double Ticks = FMath::Max<double>(A.RacingTicks, 1.0);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI pace: fastest lap %s, average best lap %s, top speed with no tow %.1f mph"),
		*LapStr(BestCount ? Fastest : NAN), *LapStr(BestCount ? BestSum / BestCount : NAN), A.CleanAirTop * MphPerMs);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI racecraft: off line %.1f%%, 3-wide %.2f%% | passes started %d (%d from the draft), completed %d (%d from the draft), aborted %d | emergency moves %d"),
		100.0 * A.OffLineTicks / Ticks, 100.0 * A.ThreeWideTicks / Ticks, A.PassStarts, A.DraftPassStarts, A.PassesDone, A.DraftPassesDone,
		A.PassesAborted, A.EmergencyMoves);
	const double St = FMath::Max<double>(A.StraightTicks, 1.0);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI held back on the straights: following %.1f%%, setting up a run %.1f%%, draft run %.1f%%, backing out %.1f%%, squeezed %.1f%%"),
		100.0 * A.StraightCapped[0] / St, 100.0 * A.StraightCapped[1] / St, 100.0 * A.StraightCapped[2] / St, 100.0 * A.StraightCapped[3] / St, 100.0 * A.StraightCapped[4] / St);
	const double Tu = FMath::Max<double>(A.TurnTicks, 1.0);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI held back in the turns: following %.1f%%, setting up a run %.1f%%, draft run %.1f%%, backing out %.1f%%, squeezed %.1f%%"),
		100.0 * A.TurnCapped[0] / Tu, 100.0 * A.TurnCapped[1] / Tu, 100.0 * A.TurnCapped[2] / Tu, 100.0 * A.TurnCapped[3] / Tu, 100.0 * A.TurnCapped[4] / Tu);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI passes completed: rookies %d (%d finished inside a turn), regulars %d (%d), veterans %d (%d)"),
		A.PassesByType[0], A.TightPassesByType[0], A.PassesByType[1], A.TightPassesByType[1], A.PassesByType[2], A.TightPassesByType[2]);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI hard braking in the turns (car-seconds): speed profile %.1f, following %.1f, draft run %.1f, backing out %.1f, squeezed %.1f"),
		A.TurnBrakeTicks[0] / 240.0, A.TurnBrakeTicks[1] / 240.0, A.TurnBrakeTicks[3] / 240.0, A.TurnBrakeTicks[4] / 240.0, A.TurnBrakeTicks[5] / 240.0);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: racing at the front after 100 s: %d lead changes, %d top-3 changes"), LeadChanges, Top3Changes);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI 3-wide after 100 s: %.2f%%"), 100.0 * A.LateThreeWide / FMath::Max<double>(A.LateTicks, 1.0));
	const double Off = FMath::Max<double>(A.OffLineTicks, 1.0);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI off-line causes: passing %.0f%%, emergency %.0f%%, avoiding %.0f%%, waiting to merge %.0f%%, returning %.0f%%, steering error %.0f%%"),
		100.0 * A.OffPassing / Off, 100.0 * A.OffEmergency / Off, 100.0 * A.OffAvoiding / Off, 100.0 * A.OffHolding / Off, 100.0 * A.OffReturning / Off, 100.0 * A.OffTracking / Off);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI passing: closing speed at pull-out %.1f m/s; abandoned: not gaining %d, too late for the turn %d, yielded on the outside %d, lane closed %d"),
		A.PassStarts ? A.ClosingAtStart / A.PassStarts : 0.0, A.AbortStall, A.AbortLate, A.AbortYield, A.AbortBlocked);
	if (FTrackSpline::IsRoadCourse())
	{
		UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI late braking down the inside: %d tried, %d made the pass"), A.OutbrakeStarts, A.OutbrakesDone);
	}
	FString Err;
	for (int32 i = 0; i < UE_ARRAY_COUNT(A.TrackErrCount); i++) if (A.TrackErrCount[i]) Err += FString::Printf(TEXT(" %d:%+.1f"), i * 50, A.TrackErrSum[i] / A.TrackErrCount[i]);
	UE_LOG(LogRacingSim, Display, TEXT("SimCheck: AI line error by s:%s"), *Err);
}

void ARacingGameMode::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	Super::AddReferencedObjects(InThis, Collector);
	CastChecked<ARacingGameMode>(InThis)->Audio.AddReferencedObjects(Collector);
}

// --------------------------------------------------------------------------- wall check

void ARacingGameMode::CarCheckTick()
{
	// One car on the oval's front straight (flat, 1 km), driven by script: a standing start at
	// full throttle (0-60 and 0-100 mph), a stop from 200 mph on the brakes (distance, peak g),
	// and full throttle from 200 and from 225 mph for 950 m (the top speed lies between where
	// the two end up); all at minimum and at maximum wing.
	if (!Watched || !Track) return;
	const FTrackSpline& Spl = Track->GetSpline();
	constexpr float Mph = 0.44704f;
	struct FTest { float Wing; int32 Kind; float V0; };   // Kind: 0 launch, 1 brake, 2 flat out
	static const FTest Tests[] = {
		{ 0.f, 0, 0.f }, { 0.f, 1, 200.f * Mph }, { 0.f, 2, 200.f * Mph }, { 0.f, 2, 225.f * Mph },
		{ 1.f, 0, 0.f }, { 1.f, 1, 200.f * Mph }, { 1.f, 2, 200.f * Mph }, { 1.f, 2, 225.f * Mph } };
	constexpr int32 NumTests = UE_ARRAY_COUNT(Tests);
	const float StartS = FTrackSpline::WrapS(-495.f);
	auto Launch = [&](int32 Run)
	{
		const FTest& T = Tests[Run];
		Watched->Core().Sim->GetAero().SetDownforceLevel(T.Wing);
		const FVector N = Spl.SurfaceNormal(StartS, 0.f);
		const FVector Fwd = Spl.Forward(StartS);
		const FVector Pos = Spl.Point(StartS, 0.f) + N * (Watched->Core().Spec().RideHeight() + 0.03f);
		Watched->SetScriptedLimits(true);
		Watched->SetScriptedThrottle(T.Kind == 1 ? 0.f : 1.f);
		Watched->SetScriptedBrake(T.Kind == 1 ? 1.f : 0.f);
		Watched->PlaceCustom(Pos, FRotationMatrix::MakeFromXZ(Fwd, N).ToQuat(), Fwd * T.V0, T.V0);
		CarRunStart = -1.0;
		CarT60 = CarT100 = NAN;
		CarPeakG = 0.f;
	};
	if (CarRun < 0)
	{
		CarRun = 0;
		Launch(0);
		return;
	}
	const FCarSnapshot C = Watched->GetSnapshot();
	if (C.PlacementCount <= CarSeenPlacements) return;
	if (CarRunStart < 0.0)
	{
		CarRunStart = C.SimTime;
		CarStartS = C.TrackS;
	}
	const FTest& T = Tests[CarRun];
	const float Time = C.SimTime - CarRunStart;
	const float Dist = FTrackSpline::DeltaS(CarStartS, C.TrackS);
	const float V = C.ForwardSpeed;
	bool bDone = false;
	FString Line;
	const TCHAR* WingName = T.Wing > 0.5f ? TEXT("max wing") : TEXT("min wing");
	static const bool bTrace = FParse::Param(FCommandLine::Get(), TEXT("CarTrace"));
	static double NextTrace = 0.0;
	if (bTrace && C.SimTime >= NextTrace)
	{
		NextTrace = C.SimTime + 0.2;
		UE_LOG(LogRacingSim, Display, TEXT("CARTRACE run %d t=%.2f v=%.1f mph gear %d rpm %.0f thr %.2f brk %.2f slip %.3f longG %.2f"), CarRun, Time, V / Mph,
			C.Gear, C.Rpm, C.Throttle, C.Brake, C.RearSlipRatio, C.LongitudinalG);
	}
	if (T.Kind == 0)
	{
		if (FMath::IsNaN(CarT60) && V >= 60.f * Mph) CarT60 = Time;
		if (FMath::IsNaN(CarT100) && V >= 100.f * Mph) CarT100 = Time;
		if (!FMath::IsNaN(CarT100) || Time > 15.f)
		{
			bDone = true;
			Line = FString::Printf(TEXT("%s: 0-60 mph %.2f s, 0-100 mph %.2f s (%.0f m)"), WingName, CarT60, CarT100, Dist);
		}
	}
	else if (T.Kind == 1)
	{
		CarPeakG = FMath::Min(CarPeakG, C.LongitudinalG);
		if (V < 0.5f || Time > 15.f)
		{
			bDone = true;
			Line = FString::Printf(TEXT("%s: 200-0 mph in %.1f m, %.2f s, peak %.1f g, average %.1f g"), WingName, Dist, Time,
				-CarPeakG, 200.f * Mph / FMath::Max(Time, 0.01f) / 9.81f);
		}
	}
	else if (Dist > 950.f || Time > 20.f)
	{
		bDone = true;
		Line = FString::Printf(TEXT("%s: flat out from %.0f mph -> %.1f mph after %.0f m (gear %d, %.0f rpm)"), WingName, T.V0 / Mph, V / Mph, Dist, C.Gear, C.Rpm);
	}
	if (!bDone) return;
	UE_LOG(LogRacingSim, Display, TEXT("CARCHECK %s"), *Line);
	CarResults.Add(Line);
	CarSeenPlacements = C.PlacementCount;
	if (++CarRun < NumTests)
	{
		Launch(CarRun);
		return;
	}
	for (const FString& L : CarResults) UE_LOG(LogRacingSim, Display, TEXT("CARCHECK summary  %s"), *L);
	FGenericPlatformMisc::RequestExit(false);
}

void ARacingGameMode::WallCheckTick()
{
	// Port of WallHitCheck: fires the car into the outer wall on the straight and in a banked
	// turn, at 40 / 70 / 100 m/s, 5 / 15 / 30 degrees, nose first / sideways / backward, and
	// reports how far it is thrown (rise of the center of mass, speed away from the surface,
	// roll, pitch) and whether it ends up over the wall or upside down.
	if (!Watched || !Track) return;
	const FTrackSpline& Spl = Track->GetSpline();
	const float Speeds[3] = { 40.f, 70.f, 100.f }, Angles[3] = { 5.f, 15.f, 30.f }, Yaws[3] = { 0.f, 90.f, 180.f };
	const float Turn = Spl.TurnStarts()[0];
	const float Places[2] = { Turn - 400.f, Turn + 180.f };
	constexpr int32 Total = 2 * 3 * 3 * 3;
	auto Decode = [&](int32 Run, float& S, float& V, float& Ang, float& Yaw, bool& bTurn)
	{
		Yaw = Yaws[Run % 3]; Run /= 3;
		Ang = Angles[Run % 3]; Run /= 3;
		V = Speeds[Run % 3]; Run /= 3;
		S = Places[Run];
		bTurn = Run == 1;
	};
	auto Launch = [&](int32 Run)
	{
		float S, V, Ang, Yaw;
		bool bTurn;
		Decode(Run, S, V, Ang, Yaw, bTurn);
		const float D = FTrackSpline::RoadOuter - 4.5f;
		const FVector N = Spl.SurfaceNormal(S, D);
		const FVector Fwd = (Spl.Forward(S) - N * FVector::DotProduct(Spl.Forward(S), N)).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(N, Fwd);   // Unreal: up x forward = right
		const float A = FMath::DegreesToRadians(Ang), H = FMath::DegreesToRadians(Ang - Yaw);
		const FVector Dir = Fwd * FMath::Cos(A) + Right * FMath::Sin(A);       // toward the wall
		const FVector BodyFwd = Fwd * FMath::Cos(H) + Right * FMath::Sin(H);   // yawed to the left
		const FQuat Q = FRotationMatrix::MakeFromXZ(BodyFwd, N).ToQuat();
		const FVector Pos = Spl.Point(S, D) + N * (Watched->Core().Spec().RideHeight() + 0.03f);
		Watched->SetScriptedThrottle(Yaw == 0.f ? 0.3f : 0.f);
		Watched->PlaceCustom(Pos, Q, Dir * V, Yaw == 0.f ? V : 0.f);
	};
	if (WallRun < 0)
	{
		UE_LOG(LogRacingSim, Display, TEXT("WALLCHECK place  v(m/s) angle yaw | maxRise(m) maxVup(m/s) maxRoll maxPitch  end"));
		WallRun = 0;
		Launch(0);
		return;
	}
	const FCarSnapshot C = Watched->GetSnapshot();
	if (C.PlacementCount <= WallSeenPlacements) return;   // the placement has not reached the physics yet
	if (WallRunStart < 0.0)
	{
		WallRunStart = C.SimTime;
		WallBase = NAN;
		WallMaxRise = WallMaxVUp = WallMaxRoll = WallMaxPitch = 0.f;
		bWallOver = false;
	}
	const FTrackSpline::FTrackPos Tp = Spl.Project(C.ComPos);
	const float Dd = FMath::Min(Tp.D, FTrackSpline::RoadOuter);
	const float H = C.ComPos.Z - Spl.Height(Tp.S, Dd);
	if (FMath::IsNaN(WallBase)) WallBase = H;
	const FVector Sn = Spl.SurfaceNormal(Tp.S, Dd);
	WallMaxRise = FMath::Max(WallMaxRise, H - WallBase);
	WallMaxVUp = FMath::Max(WallMaxVUp, (float)FVector::DotProduct(C.Vel, Sn));
	WallMaxRoll = FMath::Max(WallMaxRoll, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp((float)FVector::DotProduct(C.Up, Sn), -1.f, 1.f))));
	WallMaxPitch = FMath::Max(WallMaxPitch, FMath::Abs(FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp((float)FVector::DotProduct(C.Fwd, Sn), -1.f, 1.f)))));
	if (Tp.D > FTrackSpline::RoadOuter + 0.5f) bWallOver = true;
	static int32 TraceRun = -2;
	if (TraceRun == -2) { TraceRun = -1; FParse::Value(FCommandLine::Get(), TEXT("WallTrace="), TraceRun); }
	static double NextTrace = 0.0;
	if (WallRun == TraceRun && C.SimTime - WallRunStart >= NextTrace)
	{
		NextTrace += 0.1;
		UE_LOG(LogRacingSim, Display, TEXT("WALLTRACE t=%.2f s=%.1f d=%.2f h=%.2f upN=%.2f fwdN=%.2f v=%.1f vUp=%.2f contacts=%d  filter calls %d pairs %d flattened %d"),
			C.SimTime - WallRunStart, Tp.S, Tp.D, H - WallBase, FVector::DotProduct(C.Up, Sn), FVector::DotProduct(C.Fwd, Sn), C.Vel.Size(),
			FVector::DotProduct(C.Vel, Sn), C.Contacts, FWallContactFilter::Calls.load(), FWallContactFilter::Pairs.load(), FWallContactFilter::Flattened.load());
	}
	if (C.SimTime - WallRunStart < 4.0) return;

	float S, V, Ang, Yaw;
	bool bTurn;
	Decode(WallRun, S, V, Ang, Yaw, bTurn);
	const bool bFlip = FVector::DotProduct(C.Up, Sn) < 0.f;
	UE_LOG(LogRacingSim, Display, TEXT("WALLCHECK %-6s %5.0f %5.0f %4.0f | %8.2f %9.2f %7.0f %7.0f   %s"), bTurn ? TEXT("turn") : TEXT("str"),
		V, Ang, Yaw, WallMaxRise, WallMaxVUp, WallMaxRoll, WallMaxPitch, bWallOver ? TEXT("OVER WALL") : bFlip ? TEXT("UPSIDE DOWN") : TEXT("ok"));
	WallWorstRise = FMath::Max(WallWorstRise, WallMaxRise);
	WallWorstV = FMath::Max(WallWorstV, WallMaxVUp);
	if (bWallOver) WallOverCount++;
	else if (bFlip) WallFlipCount++;
	WallSeenPlacements = C.PlacementCount;
	WallRunStart = -1.0;
	if (++WallRun >= Total)
	{
		UE_LOG(LogRacingSim, Display, TEXT("WALLCHECK runs %d  worst rise %.2f m  worst upward speed %.2f m/s  over wall %d  upside down %d"),
			Total, WallWorstRise, WallWorstV, WallOverCount, WallFlipCount);
		bWallCheck = false;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	Launch(WallRun);
}
