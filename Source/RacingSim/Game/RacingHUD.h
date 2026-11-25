#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "RacingHUD.generated.h"

class ARacingGameMode;
struct FRaceSnapshot;
struct FCarSnapshot;

/**
 * Every screen of the game, drawn with the Canvas like the Java version's guiNode: ports of
 * Hud, MenuScreen, ResultsScreen, SplashScreen, ConfirmDialog, StandingsView and
 * SteeringWheelWidget.
 *
 * Layout uses the Java game's coordinates: a 1600 x 900 screen with the origin at the bottom
 * left and text placed by its top edge, scaled to the window's height (the width follows the
 * window's aspect ratio, so right-aligned panels stay at the right edge).
 */
UCLASS()
class RACINGSIM_API ARacingHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	// ---- drawing helpers in Java GUI coordinates ----
	void Rect(float X, float Y, float W, float H, const FLinearColor& Color);
	/** @return width of the text (GUI units) */
	float Text(const FString& S, float Size, const FLinearColor& Color, float X, float Top, bool bBold = false);
	float Centered(const FString& S, float Size, const FLinearColor& Color, float Cx, float Top, bool bBold = false);
	float Measure(const FString& S, float Size, bool bBold = false);
	/** The engine's Roboto at exactly this size (Java GUI units) and weight, rasterized at that pixel size. */
	class UFont* FontFor(float Size, bool bBold);

	void DrawSplash(ARacingGameMode* Gm, float Opacity);
	void DrawMenu(ARacingGameMode* Gm);
	void DrawRaceHud(ARacingGameMode* Gm, const FRaceSnapshot& R, const FCarSnapshot& C);
	void DrawPit(ARacingGameMode* Gm, const FRaceSnapshot& R, const FCarSnapshot& C);
	void DrawStartLights(const FRaceSnapshot& R);
	void DrawSteeringWheel(float Steer, bool bMouseSteering);
	void DrawResults(ARacingGameMode* Gm, const FRaceSnapshot& R);
	void DrawConfirm(ARacingGameMode* Gm);
	void Arc(float Cx, float Cy, float RIn, float ROut, float A0, float A1, int32 Segments, const FLinearColor& Inner, const FLinearColor& Outer);

	float Scale = 1.f;
	float VW = 1600.f, VH = 900.f;
	float Alpha = 1.f;
	float WheelRotation = 0.f;

	/** The splash key art and the engine's Roboto: members, so garbage collection keeps them loaded. */
	UPROPERTY()
	TObjectPtr<class UTexture2D> SplashArt;
	UPROPERTY()
	TObjectPtr<class UFont> Roboto;

	/** Font copies keyed by point size * 2 + bold. */
	UPROPERTY()
	TMap<int32, TObjectPtr<class UFont>> Fonts;
};
