#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Sim/RaceLogic.h"
#include "Audio/RaceAudio.h"
#include "RacingGameMode.generated.h"

class AOvalTrack;
class ARaceCar;
class ARaceDirector;
class ACameraActor;
class AScenery;
class ATireSmoke;

UENUM()
enum class ERaceState : uint8 { Loading, Menu, Race, Results };

/** Race setup chosen in the menu (port of com.racingsim.ui.MenuScreen's state). */
struct FMenuSettings
{
	static constexpr int32 RaceLengths[2] = { 5, 10 };
	/** The road course's laps are almost three times as long as the oval's. */
	static constexpr int32 RoadRaceLengths[2] = { 3, 5 };
	static constexpr int32 MinOpponents = 1, MaxOpponents = 32;
	static constexpr int32 MaxPitStops = 3;
	static constexpr int32 DownforceSteps = 10;
	static constexpr int32 Rows = 6, TrackRow = 0, PitRow = 4, DownforceRow = 5;
	/** The wing a track starts you on: none on the oval, most of it on the road course. */
	static int32 DefaultDownforce(ETrackLayout L) { return L == ETrackLayout::Oval ? 0 : 8; }

	ETrackLayout Track = ETrackLayout::Oval;
	int32 Row = 0;
	int32 LapIndex = 0;
	int32 CustomLaps = -1;
	int32 Opponents = 7;
	int32 StartPosition = 6;
	int32 PitStops = 0;
	int32 Downforce = 0;

	int32 Laps() const { return CustomLaps > 0 ? CustomLaps : (Track == ETrackLayout::Oval ? RaceLengths : RoadRaceLengths)[LapIndex]; }
	/** Pit stops are only run on the oval (it has the pit lane). */
	bool HasPits() const { return Track == ETrackLayout::Oval; }
	int32 FieldSize() const { return Opponents + 1; }
	/** Left / Right on the selected row. @return true if the field (cars / grid) changed (a
	 *  change of track is seen by the caller from the row) */
	bool Change(int32 Delta);
};

/**
 * The game flow (port of com.racingsim.app.RaceApp): a splash screen while the track and the
 * field are built step by step, the race setup menu over the field lined up on the grid, the
 * rolling-start race, the results, and the Esc prompts (leave the race / quit the game). The
 * screens themselves are drawn by ARacingHUD.
 *
 * Command line:
 *   -Opponents=7 -Start=6 -Laps=5 -Pits=0 -Downforce=0   preset the menu
 *   -Track=road         race on the road course (standing start) instead of the oval; the
 *                       menu's TRACK row reloads the level with ?Track= and the other settings
 *   -QuickStart         skip the menu and start the race straight away
 *   -Autopilot          the AI drives your car
 *   -SimCheck           headless check like the Java `gradlew simCheck -Pplayer=ai`: an all-AI
 *                       field (-Cars=8), the 6th car is watched; prints telemetry and the
 *                       classification, then quits (-Seconds=260 limit)
 *   -CarCheck           headless performance check: 0-60 / 0-100 mph, the stop from 200 mph and
 *                       the top speed, at minimum and maximum wing, then quits
 *   -AutoShot=6,12,20   screenshots (with the HUD) at these race times, then quit; with -ShotMenu
 *                       the times count from the menu instead (-ShotCam=tcam for the T-cam)
 */
UCLASS()
class RACINGSIM_API ARacingGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARacingGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);

	// ---- read by the HUD ----
	ERaceState GetState() const { return State; }
	const FMenuSettings& GetMenu() const { return Menu; }
	ARaceDirector* GetDirector() const { return Director; }
	ARaceCar* GetPlayerCar() const { return Watched; }
	bool IsConfirmOpen() const { return bConfirmOpen; }
	void GetConfirmText(FString& Title, FString& Message, FString& EnterText, FString& EscText) const;
	float GetSplashProgress() const { return SplashShown; }
	const FString& GetSplashStatus() const { return SplashStatus; }
	/** 1 = splash fully shown, 0 = gone. */
	float GetSplashOpacity() const;
	bool IsHelpShown() const { return bShowHelp; }

private:
	void LoadingTick(float Dt);
	void BuildField();
	void BuildSimCheckField(int32 CarCount, int32 Downforce);
	ARaceCar* SpawnCar(int32 Slot, int32 CarCount, int32 LiveryIndex, bool bHuman, const FString& NameOverride);
	void AiParams(int32 Index, float& Grip, float& Aggression);
	static float AiGripBase();
	void EnterMenu();
	void StartRace();
	void ShowResults();
	void OpenConfirm();
	void CloseConfirm(bool bResume);
	void AcceptConfirm();
	void HandleInput(float Dt);
	void ChangeSetting(int32 Dir);
	/** Reloads the level on the track chosen in the menu, keeping the other settings. */
	void ReloadForTrack();
	bool bReloading = false;
	void OrbitGrid(float Dt);
	void ToggleQuality();
	void AutomationTick();
	void LogClassification();
	void WallCheckTick();
	void CarCheckTick();
	class APlayerController* Pc() const;

	UPROPERTY()
	TObjectPtr<AOvalTrack> Track;

	UPROPERTY()
	TObjectPtr<ARaceDirector> Director;

	UPROPERTY()
	TObjectPtr<ARaceCar> Watched;

	UPROPERTY()
	TArray<TObjectPtr<ARaceCar>> FieldCars;

	UPROPERTY()
	TObjectPtr<ACameraActor> MenuCam;

	UPROPERTY()
	TObjectPtr<AScenery> Scenery;

	UPROPERTY()
	TObjectPtr<class ASkyLight> SkyLight;

	UPROPERTY()
	TObjectPtr<ATireSmoke> Smoke;

	FRaceAudio Audio;

	// car check (-CarCheck): launch, braking and top speed on the oval's front straight
	bool bCarCheck = false;
	int32 CarRun = -1;
	int32 CarSeenPlacements = 0;
	double CarRunStart = -1.0;
	float CarStartS = 0.f, CarT60 = NAN, CarT100 = NAN, CarPeakG = 0.f;
	TArray<FString> CarResults;

	// wall check (-WallCheck): port of WallHitCheck
	bool bWallCheck = false;
	int32 WallRun = -1;
	int32 WallSeenPlacements = 0;
	double WallRunStart = -1.0;
	float WallBase = NAN, WallMaxRise = 0.f, WallMaxVUp = 0.f, WallMaxRoll = 0.f, WallMaxPitch = 0.f;
	bool bWallOver = false;
	float WallWorstRise = 0.f, WallWorstV = 0.f;
	int32 WallOverCount = 0, WallFlipCount = 0;

	ERaceState State = ERaceState::Loading;
	FMenuSettings Menu;
	bool bConfirmOpen = false;
	bool bShowHelp = true;
	bool bHighQuality = true;
	float FinishedFor = 0.f;
	float OrbitAngle = 0.f;
	int32 HeldDirection = 0;
	float HeldTime = 0.f, RepeatTimer = 0.f;

	// loading / splash
	int32 LoadStep = 0;
	int32 WarmupFrames = 0;
	float SplashTarget = 0.f, SplashShown = 0.f;
	FString SplashStatus;
	float SplashFade = 0.f;

	// AI drivers' grip and aggression: java.util.Random(42) in creation order, as in RaceApp
	FJavaRandom AiRandom{ 42 };
	TArray<FVector2f> AiParamPool;

	// command-line options
	bool bAutopilot = false;
	bool bQuickStart = false;
	bool bSimCheck = false;
	int32 SimCars = 8;
	float CheckSeconds = 260.f;
	bool bShotMenu = false;
	bool bShotTCam = false;
	bool bShotShowcase = false;
	bool bShotNoHud = false;
	TArray<float> ShotTimes;
	int32 NextShot = 0;
	double StateStartTime = 0.0;
	double NextLogTime = 0.0;
	double StartSimTime = -1.0;
	float WatchedTop = 0.f;
	TArray<FString> LastTop3, PendingTop3;
	double PendingSince = 0.0;
	int32 LeadChanges = 0, Top3Changes = 0;
	double NextFrontLog = 0.0;
	double AllFinishedAt = -1.0;
};
