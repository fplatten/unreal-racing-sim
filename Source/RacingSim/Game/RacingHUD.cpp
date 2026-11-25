#include "Game/RacingHUD.h"
#include "Car/RacingController.h"
#include "Car/RaceCar.h"
#include "Game/RaceDirector.h"
#include "Game/RacingGameMode.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

namespace
{
	/** jME's default GUI font size (pixels at 1600 x 900). */
	constexpr float Base = 17.f;
	constexpr float MsToMph = 2.23694f;

	const FLinearColor White(1, 1, 1, 1);
	const FLinearColor Black(0, 0, 0, 1);
	const FLinearColor Yellow(1, 1, 0, 1);
	const FLinearColor Panel(0.02f, 0.03f, 0.06f, 0.62f);
	const FLinearColor DraftOn(0.2f, 0.95f, 1.0f, 1);
	const FLinearColor Dim(0.6f, 0.62f, 0.66f, 1);
	const FLinearColor Go(0.3f, 1.f, 0.35f, 1);
	const FLinearColor Stop(1.f, 0.25f, 0.2f, 1);
	const FLinearColor PitYellow(1.f, 0.86f, 0.15f, 1);
	const FLinearColor Accent(0.10f, 0.32f, 0.85f, 0.9f);
	const FLinearColor Gold(1.f, 0.85f, 0.3f, 1);

	FString FormatTime(float T)
	{
		if (FMath::IsNaN(T)) return TEXT("--:--.---");
		const int32 Min = (int32)(T / 60);
		const float Sec = T - Min * 60;
		return FString::Printf(TEXT("%d:%06.3f"), Min, Sec);
	}

	/**
	 * Rows of the race order to show when the field is bigger than the panel: the leaders, a gap
	 * marker (INDEX_NONE) and a window around the player, so the player's row is always visible.
	 */
	TArray<int32> StandingsRows(int32 N, int32 Mine, int32 MaxRows)
	{
		TArray<int32> Out;
		if (N <= MaxRows || Mine < MaxRows - 1)
		{
			for (int32 i = 0; i < FMath::Min(N, MaxRows); i++) Out.Add(i);
			if (N > MaxRows && Mine >= MaxRows) Out[MaxRows - 1] = Mine;
			return Out;
		}
		const int32 Leaders = 3;
		const int32 Window = MaxRows - Leaders - 1;
		const int32 From = FMath::Max(Leaders, FMath::Min(Mine - Window / 2, N - Window));
		for (int32 i = 0; i < Leaders; i++) Out.Add(i);
		Out.Add(INDEX_NONE);
		for (int32 i = From; i < From + Window; i++) Out.Add(i);
		return Out;
	}
}

// ------------------------------------------------------------------------------- helpers

namespace
{
	/** The Java colors are sRGB; the Canvas works in linear color. */
	FLinearColor Lin(const FLinearColor& C, float Alpha)
	{
		auto L = [](float X) { return X <= 0.04045f ? X / 12.92f : FMath::Pow((X + 0.055f) / 1.055f, 2.4f); };
		return FLinearColor(L(C.R), L(C.G), L(C.B), C.A * Alpha);
	}
}

UFont* ARacingHUD::FontFor(float Size, bool bBold)
{
	// Size is the Java font size (its line height in pixels at 900 lines). Scaling a small font up
	// blurs it, so every size gets its own copy of Roboto rasterized at that pixel size: the font
	// cache renders runtime fonts at LegacyFontSize points (96 DPI: 4/3 pixels per point).
	if (!Roboto) Roboto = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto"));
	if (!Roboto) return GEngine ? GEngine->GetLargeFont() : nullptr;
	const int32 Points = FMath::Clamp(FMath::RoundToInt(Size * Scale * 0.80f * 0.75f), 4, 400);
	const int32 Key = Points * 2 + (bBold ? 1 : 0);
	if (TObjectPtr<UFont>* Found = Fonts.Find(Key)) return *Found;
	UFont* F = DuplicateObject<UFont>(Roboto, this);
	F->LegacyFontSize = Points;
	F->LegacyFontName = bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular"));
	Fonts.Add(Key, F);
	return F;
}

void ARacingHUD::Rect(float X, float Y, float W, float H, const FLinearColor& Color)
{
	const FLinearColor C = Lin(Color, Alpha);
	FCanvasTileItem Tile(FVector2D(X * Scale, (VH - Y - H) * Scale), FVector2D(W * Scale, H * Scale), C);
	Tile.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Tile);
}

float ARacingHUD::Measure(const FString& S, float Size, bool bBold)
{
	UFont* F = FontFor(Size, bBold);
	if (!F) return S.Len() * Size * 0.5f;
	return F->GetStringSize(*S) / Scale;
}

float ARacingHUD::Text(const FString& S, float Size, const FLinearColor& Color, float X, float Top, bool bBold)
{
	UFont* F = FontFor(Size, bBold);
	if (!F) return 0.f;
	// whole pixels: text placed between pixels is filtered and looks soft
	FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(X * Scale), FMath::RoundToFloat((VH - Top) * Scale)), FText::FromString(S), F, Lin(Color, Alpha));
	Canvas->DrawItem(Item);
	return F->GetStringSize(*S) / Scale;
}
float ARacingHUD::Centered(const FString& S, float Size, const FLinearColor& Color, float Cx, float Top, bool bBold)
{
	const float W = Measure(S, Size, bBold);
	Text(S, Size, Color, Cx - W / 2, Top, bBold);
	return W;
}

void ARacingHUD::Arc(float Cx, float Cy, float RIn, float ROut, float A0, float A1, int32 Segments, const FLinearColor& Inner, const FLinearColor& Outer)
{
	// annular sector between angles A0..A1 (degrees clockwise from 12 o'clock, rotated with the
	// wheel), with an inner -> outer color gradient; Cx, Cy in GUI units (y up)
	TArray<FCanvasUVTri> Tris;
	auto P = [&](float R, float ADeg)
	{
		const float A = FMath::DegreesToRadians(ADeg + WheelRotation);
		return FVector2D((Cx + FMath::Sin(A) * R) * Scale, (VH - (Cy + FMath::Cos(A) * R)) * Scale);
	};
	const FLinearColor In = Lin(Inner, Alpha), Out = Lin(Outer, Alpha);
	for (int32 i = 0; i < Segments; i++)
	{
		const float Aa = A0 + (A1 - A0) * i / Segments, Ab = A0 + (A1 - A0) * (i + 1) / Segments;
		FCanvasUVTri T1, T2;
		T1.V0_Pos = P(RIn, Aa); T1.V1_Pos = P(ROut, Aa); T1.V2_Pos = P(ROut, Ab);
		T1.V0_Color = In; T1.V1_Color = Out; T1.V2_Color = Out;
		T2.V0_Pos = P(RIn, Aa); T2.V1_Pos = P(ROut, Ab); T2.V2_Pos = P(RIn, Ab);
		T2.V0_Color = In; T2.V1_Color = Out; T2.V2_Color = In;
		Tris.Add(T1);
		Tris.Add(T2);
	}
	FCanvasTriangleItem Item(Tris, GWhiteTexture);
	Item.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Item);
}

// --------------------------------------------------------------------------------- frame

void ARacingHUD::DrawHUD()
{
	Super::DrawHUD();
	ARacingGameMode* Gm = GetWorld() ? GetWorld()->GetAuthGameMode<ARacingGameMode>() : nullptr;
	if (!Gm || !Canvas) return;
	Scale = Canvas->ClipY / 900.f;
	VH = 900.f;
	VW = Canvas->ClipX / Scale;
	Alpha = 1.f;

	const ERaceState State = Gm->GetState();
	if (State != ERaceState::Loading)
	{
		if (State == ERaceState::Menu)
		{
			DrawMenu(Gm);
		}
		else if (Gm->GetDirector() && Gm->GetPlayerCar())
		{
			const FRaceSnapshot R = Gm->GetDirector()->GetSnapshot();
			const FCarSnapshot C = Gm->GetPlayerCar()->GetSnapshot();
			if (R.Standings.Num() > 0) DrawRaceHud(Gm, R, C);
			if (State == ERaceState::Results) DrawResults(Gm, R);
		}
		if (Gm->IsConfirmOpen()) DrawConfirm(Gm);
	}
	const float Splash = Gm->GetSplashOpacity();
	if (Splash > 0.f) DrawSplash(Gm, Splash);
}

// -------------------------------------------------------------------------------- splash

void ARacingHUD::DrawSplash(ARacingGameMode* Gm, float Opacity)
{
	// full-screen key art (dimmed), the title and a loading bar with the step being loaded
	Alpha = Opacity;
	Rect(0, 0, VW, VH, Black);
	if (!SplashArt) SplashArt = LoadObject<UTexture2D>(nullptr, TEXT("/Game/Textures/UI/splash.splash"));
	UTexture2D* Art = SplashArt;
	if (Art && Art->GetResource())
	{
		const float Iw = Art->GetSizeX(), Ih = Art->GetSizeY();
		const float S = FMath::Max(VW / Iw, VH / Ih);   // cover the screen, keeping the aspect ratio
		const float W = Iw * S, H = Ih * S;
		FCanvasTileItem Tile(FVector2D((VW - W) / 2 * Scale, (VH - H) / 2 * Scale), Art->GetResource(), FVector2D(W * Scale, H * Scale),
			Lin(FLinearColor(0.62f, 0.62f, 0.62f, 1.f), Alpha));
		Tile.BlendMode = SE_BLEND_Translucent;
		Canvas->DrawItem(Tile);
	}
	Rect(0, 0, VW, 230, FLinearColor(0, 0, 0, 0.72f));
	Rect(0, VH * 0.58f, VW, 200, FLinearColor(0, 0, 0, 0.35f));
	Centered(TEXT("OVAL RACING SIM"), Base * 4.4f, White, VW / 2, VH * 0.58f + 160, true);
	Centered(TEXT("2.5-MILE OVAL   -   OPEN-WHEEL RACING   -   33-CAR FIELD"), Base * 1.1f, FLinearColor(1.f, 0.85f, 0.35f, 1), VW / 2, VH * 0.58f + 62);

	const float BarW = 640.f, BarH = 12.f, BarX = (VW - BarW) / 2, BarY = 120;
	const float Progress = Gm->GetSplashProgress();
	Rect(BarX - 3, BarY - 3, BarW + 6, BarH + 6, FLinearColor(1, 1, 1, 0.18f));
	Rect(BarX, BarY, FMath::Max(1.f, BarW * Progress), BarH, FLinearColor(1.f, 0.80f, 0.20f, 1));
	Text(Gm->GetSplashStatus(), Base, FLinearColor(0.80f, 0.82f, 0.86f, 1), BarX, BarY + BarH + 34);
	const FString Pct = FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Progress * 100));
	Text(Pct, Base, White, BarX + BarW - Measure(Pct, Base), BarY + BarH + 34);
	Centered(TEXT("Unreal Engine  -  Chaos physics  -  assets made in Blender"), Base * 0.8f, FLinearColor(0.6f, 0.62f, 0.66f, 1), VW / 2, 52);
	Alpha = 1.f;
}

// ---------------------------------------------------------------------------------- menu

void ARacingHUD::DrawMenu(ARacingGameMode* Gm)
{
	const FMenuSettings& M = Gm->GetMenu();
	const FLinearColor MenuPanel(0.02f, 0.03f, 0.06f, 0.72f), Idle(1, 1, 1, 0.07f), MenuDim(0.70f, 0.72f, 0.76f, 1);
	const float Pw = 660, Ph = 708, Px = (VW - Pw) / 2, Py = (VH - Ph) / 2;
	Rect(Px, Py, Pw, Ph, MenuPanel);
	Rect(Px, Py + Ph - 6, Pw, 6, Accent);
	Centered(TEXT("OVAL RACING SIM"), Base * 3.2f, White, VW / 2, Py + Ph - 30, true);
	Centered(TEXT("2.5-mile oval  -  open-wheel cars  -  rolling start, three abreast"), Base, MenuDim, VW / 2, Py + Ph - 110);
	Centered(TEXT("RACE SETUP"), Base * 1.2f, Gold, VW / 2, Py + Ph - 160, true);

	const TCHAR* Labels[] = { TEXT("RACE LENGTH"), TEXT("OPPONENTS"), TEXT("STARTING POSITION"), TEXT("PIT STOPS"), TEXT("DOWNFORCE") };
	const FString Values[] = {
		FString::Printf(TEXT("%d LAPS"), M.Laps()),
		FString::Printf(TEXT("%d   (%d cars)"), M.Opponents, M.FieldSize()),
		FString::Printf(TEXT("P%d of %d"), M.StartPosition, M.FieldSize()),
		M.PitStops == 0 ? FString(TEXT("NONE")) : FString::Printf(TEXT("%d %s"), M.PitStops, M.PitStops == 1 ? TEXT("STOP") : TEXT("STOPS")),
		FString() };
	const float BoxW = 560, BoxH = 54, X = VW / 2 - BoxW / 2;
	for (int32 i = 0; i < FMenuSettings::Rows; i++)
	{
		const bool bOn = i == M.Row;
		const float Y = Py + Ph - 250 - i * (BoxH + 14);
		Rect(X, Y, BoxW, BoxH, bOn ? Accent : Idle);
		Text(Labels[i], Base * 1.15f, bOn ? White : MenuDim, X + 22, Y + BoxH - 14);
		if (i != FMenuSettings::DownforceRow)
		{
			const FString V = bOn ? TEXT("<  ") + Values[i] + TEXT("  >") : Values[i];
			Centered(V, Base * 1.4f, bOn ? White : MenuDim, X + BoxW - 130, Y + BoxH - 10);
			continue;
		}
		// slider: SPEED (minimum downforce) .. CONTROL (maximum downforce)
		const float SliderW = 180.f, SliderX = X + 285, SliderY = Y + BoxH / 2.f;
		const float Frac = M.Downforce / (float)FMenuSettings::DownforceSteps;
		Text(TEXT("SPEED"), Base * 0.85f, M.Downforce == 0 ? Gold : bOn ? White : MenuDim,
			SliderX - 14 - Measure(TEXT("SPEED"), Base * 0.85f), SliderY + Base * 0.45f);
		Text(TEXT("CONTROL"), Base * 0.85f, M.Downforce == FMenuSettings::DownforceSteps ? Gold : bOn ? White : MenuDim,
			SliderX + SliderW + 14, SliderY + Base * 0.45f);
		Rect(SliderX, SliderY - 3, SliderW, 6, FLinearColor(1, 1, 1, 0.25f));
		for (int32 k = 0; k <= FMenuSettings::DownforceSteps; k++)
		{
			Rect(SliderX + SliderW * k / FMenuSettings::DownforceSteps - 1, SliderY - 7, 2, 14, FLinearColor(1, 1, 1, 0.3f));
		}
		Rect(SliderX, SliderY - 3, FMath::Max(1.f, SliderW * Frac), 6, Gold);
		Rect(SliderX + SliderW * Frac - 6, SliderY - 13, 12, 26, bOn ? White : MenuDim);
	}
	Centered(TEXT("UP / DOWN choose a setting  -  LEFT / RIGHT change it  -  ENTER start the race"), Base, White, VW / 2, Py + 160);
	Centered(TEXT("W/Up throttle   S/Down brake   A/D steer (or click mouse wheel: mouse steering)   E/Q shift"), Base * 0.85f, MenuDim, VW / 2, Py + 115);
	Centered(TEXT("T auto gearbox   R reset to start line   C camera   G graphics quality   H help   Esc quit game"), Base * 0.85f, MenuDim, VW / 2, Py + 87);
	Centered(TEXT("Pit stops: stop in your yellow box (pit lane off turn 4) and stay put for the 8 s countdown."), Base * 0.85f, MenuDim, VW / 2, Py + 59);
	Centered(TEXT("Race in the draft: tuck in behind a car to cut drag and slingshot past."), Base * 0.85f, MenuDim, VW / 2, Py + 31);

	// racing wheel: what is connected and what it reads, so the axes and buttons can be checked
	// and set up (controller.properties next to RacingSim.exe)
	const FRacingController& Wheel = FRacingController::Get();
	if (Wheel.IsConnected())
	{
		Rect(Px, Py - 78, Pw, 70, MenuPanel);
		Centered(FString::Printf(TEXT("WHEEL  %s  -  %s"), *Wheel.GetName(), *Wheel.Describe()), Base * 0.8f, White, VW / 2, Py - 14);
		const int32 Last = Wheel.GetLastButton();
		Centered(FString::Printf(TEXT("steer %d%%    throttle %d%%    brake %d%%    last button pressed: %s"),
			FMath::RoundToInt(-Wheel.GetSteer() * 100.f), FMath::RoundToInt(Wheel.GetThrottle() * 100.f), FMath::RoundToInt(Wheel.GetBrake() * 100.f),
			Last >= 0 ? *FString::FromInt(Last) : TEXT("-")), Base * 0.8f, MenuDim, VW / 2, Py - 44);
	}
}

// ------------------------------------------------------------------------------ race HUD

void ARacingHUD::DrawRaceHud(ARacingGameMode* Gm, const FRaceSnapshot& R, const FCarSnapshot& C)
{
	ARaceCar* Car = Gm->GetPlayerCar();
	const FString MyName = Car->Core().Name;
	int32 Mine = R.Standings.IndexOfByPredicate([&MyName](const FRaceSnapshot::FRow& Row) { return Row.Name == MyName; });
	if (Mine == INDEX_NONE) return;
	const FRaceSnapshot::FRow& Me = R.Standings[Mine];
	const FVehicleSpec& Spec = Car->Core().Spec();

	// ---- telemetry panel (bottom-left) ----
	const float Px = 20, Py = 20, Pw = 340, Ph = 250;
	Rect(Px, Py, Pw, Ph, Panel);
	Text(FString::Printf(TEXT("%3d"), FMath::RoundToInt(FMath::Abs(C.ForwardSpeed) * MsToMph)), Base * 3.0f, White, Px + 16, Py + Ph - 8, true);
	Text(TEXT("MPH"), Base * 1.1f, Dim, Px + 170, Py + Ph - 40);
	Text(C.Gear == 0 ? FString(TEXT("R")) : FString::FromInt(C.Gear), Base * 3.0f, Yellow, Px + 250, Py + Ph - 8, true);
	Text(C.bAutomatic ? TEXT("AUTO") : TEXT("MANUAL"), Base * 0.85f, Dim, Px + 232, Py + Ph - 70);
	Text(FString::Printf(TEXT("RPM %6s"), *FText::AsNumber(FMath::RoundToInt(C.Rpm)).ToString()), Base, White, Px + 16, Py + Ph - 88);
	const float RpmBarW = 300.f;
	const float Frac = FMath::Clamp(C.Rpm / Spec.LimiterRpm, 0.f, 1.f);
	Rect(Px + 16, Py + Ph - 128, RpmBarW, 12, FLinearColor(1, 1, 1, 0.12f));
	const FLinearColor RpmColor = C.Rpm > Spec.RedlineRpm * 0.97f ? FLinearColor(1, 0, 0, 1)
		: C.Rpm > Spec.RedlineRpm * 0.85f ? FLinearColor(1, 0.65f, 0, 1) : FLinearColor(0, 1, 0, 1);
	Rect(Px + 16, Py + Ph - 128, FMath::Max(1.f, RpmBarW * Frac), 12, RpmColor);
	Text(FString::Printf(TEXT("LAT G %+5.2f    LONG G %+5.2f"), C.LateralG, C.LongitudinalG), Base, White, Px + 16, Py + Ph - 140);
	Text(FString::Printf(TEXT("TIRE SLIP  long %4.1f%%   lat %4.1f deg"), C.RearSlipRatio * 100.f, FMath::RadiansToDegrees(C.FrontSlipAngle)),
		Base, White, Px + 16, Py + Ph - 172);
	Text(FString::Printf(TEXT("CAM: %s     DOWNFORCE %d/%d"), Car->IsTCam() ? TEXT("T-CAM") : TEXT("CHASE"),
		FMath::RoundToInt(C.DownforceLevel * FMenuSettings::DownforceSteps), FMenuSettings::DownforceSteps), Base * 0.85f, Dim, Px + 16, Py + Ph - 208);

	// ---- race panel (top-right) ----
	const float Rw = 400, Rh = 400, Rx = VW - Rw - 20, Ry = VH - Rh - 20;
	Rect(Rx, Ry, Rw, Rh, Panel);
	Text(FString::Printf(TEXT("P%d / %d"), Me.Position, R.Standings.Num()), Base * 2.6f, White, Rx + 16, Ry + Rh - 8, true);
	FString LapText;
	FLinearColor LapColor = White;
	if (Me.bFinished) { LapText = TEXT("FINISHED"); LapColor = Yellow; }
	else if (Me.Lap >= R.RaceLaps) { LapText = TEXT("FINAL LAP"); LapColor = ((int32)(R.Now * 2)) % 2 == 0 ? White : Yellow; }
	else LapText = FString::Printf(TEXT("LAP %d/%d"), Me.Lap, R.RaceLaps);
	Text(LapText, Base * 1.3f, LapColor, Rx + 190, Ry + Rh - 18, true);
	Text(TEXT("TIME  ") + FormatTime(Me.CurrentLapTime), Base, White, Rx + 16, Ry + Rh - 76);
	if (R.RequiredStops > 0)
	{
		Text(FString::Printf(TEXT("PITS %d/%d"), Me.PitStops, R.RequiredStops), Base, Me.bStopsMet ? Go : PitYellow, Rx + 200, Ry + Rh - 76);
	}
	else if (Me.PitStops > 0)
	{
		Text(FString::Printf(TEXT("PITS %d"), Me.PitStops), Base, Dim, Rx + 200, Ry + Rh - 76);
	}
	Text(TEXT("LAST  ") + FormatTime(Me.LastLap), Base, Dim, Rx + 16, Ry + Rh - 102);
	Text(TEXT("BEST  ") + FormatTime(Me.BestLap), Base, Dim, Rx + 16, Ry + Rh - 128);

	// standings with gaps (distance over the player's speed)
	const float Top = Ry + Rh - 164, LineH = Base * 0.95f * 1.3f;
	const float RefSpeed = FMath::Max(FMath::Abs(C.ForwardSpeed), 20.f);
	const TArray<int32> Rows = StandingsRows(R.Standings.Num(), Mine, 10);
	for (int32 i = 0; i < Rows.Num(); i++)
	{
		const float Y = Top - i * LineH;
		if (Rows[i] == INDEX_NONE)
		{
			Text(TEXT("   ..."), Base * 0.95f, White, Rx + 16, Y);
			continue;
		}
		const FRaceSnapshot::FRow& E = R.Standings[Rows[i]];
		const bool bMe = Rows[i] == Mine;
		if (bMe) Rect(Rx + 8, Y - LineH - 2, Rw - 16, LineH + 1, FLinearColor(0.10f, 0.32f, 0.85f, 0.75f));
		const FString Gap = bMe ? FString(TEXT("  ----")) : FString::Printf(TEXT("%+6.2fs"), (Me.TotalDistance - E.TotalDistance) / RefSpeed);
		Text(FString::Printf(TEXT("P%d %s %s"), E.Position, bMe ? TEXT(">") : TEXT(" "), *E.Name), Base * 0.95f, White, Rx + 16, Y);
		// driver type and wing level: rookie / regular / veteran, W0 (light) .. W10 (heavy)
		static const TCHAR* Types[] = { TEXT("ROOKIE"), TEXT("REGULAR"), TEXT("VETERAN") };
		static const FLinearColor TypeColors[] = { FLinearColor(0.55f, 0.85f, 1.f, 1), FLinearColor(0.80f, 0.80f, 0.80f, 1), FLinearColor(1.f, 0.62f, 0.25f, 1) };
		if (E.DriverType >= 0) Text(Types[E.DriverType], Base * 0.75f, bMe ? White : TypeColors[E.DriverType], Rx + 158, Y - 2);
		Text(FString::Printf(TEXT("W%d"), E.Wing), Base * 0.75f, bMe ? White : FLinearColor(0.80f, 0.80f, 0.80f, 1), Rx + 248, Y - 2);
		Text(Gap, Base * 0.95f, White, Rx + 300, Y);
	}

	// ---- draft indicator (top-center) ----
	const float DraftW = 260.f, Dx = VW / 2 - DraftW / 2, Dy = VH - 70;
	const bool bActive = C.DraftFactor > 0.01f;
	Rect(Dx - 14, Dy - 22, DraftW + 28, 70, Panel);
	Text(FString::Printf(TEXT("DRAFT %2d%%"), FMath::RoundToInt(C.DragReduction * 100.f)), Base * 1.4f, bActive ? DraftOn : Dim, Dx, Dy + 42, true);
	Rect(Dx, Dy - 10, DraftW, 12, FLinearColor(1, 1, 1, 0.12f));
	if (bActive) Rect(Dx, Dy - 10, FMath::Max(1.f, DraftW * FMath::Clamp(C.DraftFactor / Drafting::MaxDraft, 0.f, 1.f)), 12, DraftOn);

	// ---- help line (under the draft meter) ----
	if (Gm->IsHelpShown())
	{
		const FString Help = ARaceCar::IsControllerConnected()
			? TEXT("Controller:  wheel/stick steer  pedals/triggers gas/brake  RB/LB shift  X auto  Back reset  Y camera  Start menu   (keys also work, H hides help)")
			: TEXT("W/Up throttle  S/Down brake  A/D or mouse steer  E/Q shift  T auto  R reset  C camera  G quality  H help  Esc menu");
		Centered(Help, Base * 0.85f, Dim, VW / 2, VH - 104);
	}

	DrawPit(Gm, R, C);
	DrawSteeringWheel(C.SteerInput, Car->IsMouseSteering());
}

void ARacingHUD::DrawPit(ARacingGameMode* Gm, const FRaceSnapshot& R, const FCarSnapshot& C)
{
	const FRaceSnapshot::FRow* Me = R.Standings.FindByPredicate([](const FRaceSnapshot::FRow& Row) { return Row.bPlayer; });
	if (!Me) return;
	FString Msg, Sub;
	FLinearColor Color = PitYellow;
	float BarFrac = -1.f;
	switch (R.PitStatus)
	{
	case 1:
		Msg = FString::Printf(TEXT("PIT STOP  %.1f"), R.PitCountdown);
		Sub = TEXT("Stay in your box until the countdown reaches zero");
		BarFrac = R.PitCountdown / FPitStop::ServiceTime;
		break;
	case 2:
		if (R.PitStatusTime < 3.f)
		{
			Msg = TEXT("PIT STOP COMPLETE  -  GO GO GO!");
			Color = Go;
			if (R.RequiredStops > 0) Sub = FString::Printf(TEXT("Pit stops %d of %d"), Me->PitStops, R.RequiredStops);
		}
		break;
	case 3:
		if (R.PitStatusTime < 4.f)
		{
			Msg = TEXT("PIT STOP DOES NOT COUNT");
			Color = Stop;
			Sub = TEXT("You left the box before the countdown reached zero");
		}
		break;
	default:
		break;
	}
	const float Rel = PitLane::Rel(C.TrackS);
	const bool bInPitLane = PitLane::InPitRoad(C.TrackS) && C.TrackD < FTrackSpline::InnerWall;
	const float BoxEnd = PitLane::StallStart(R.PitStall) + PitLane::StallLength;
	if (Msg.IsEmpty() && bInPitLane && Rel < BoxEnd && !Me->bFinished)
	{
		Msg = TEXT("PIT LANE");
		Sub = FString::Printf(TEXT("Your box is the yellow one, number %d on the start/finish line - stop inside it"), R.PitStall + 1);
	}
	if (Msg.IsEmpty()) return;
	const float Y = VH * 0.72f;
	Centered(Msg, Base * 2.2f, Color, VW / 2, Y, true);
	if (!Sub.IsEmpty()) Centered(Sub, Base, White, VW / 2, Y - Base * 2.6f);
	if (BarFrac >= 0.f)
	{
		const float BarW = 360.f, Bx = VW / 2 - BarW / 2, By = Y - Base * 4.6f;
		Rect(Bx, By, BarW, 10, FLinearColor(1, 1, 1, 0.15f));
		Rect(Bx, By, FMath::Max(1.f, BarW * BarFrac), 10, PitYellow);
	}
}

void ARacingHUD::DrawSteeringWheel(float Steer, bool bMouseSteering)
{
	// bottom center: the hub sits below the screen edge so only 10 to 2 o'clock shows; the
	// whole wheel turns with the steering input (90 degrees at full lock, left = counter-clockwise)
	const float Radius = 300.f, Cx = VW / 2, Cy = -Radius / 2;
	const float RimW = Radius * 0.11f, RIn = Radius - RimW;
	WheelRotation = -FMath::Clamp(Steer, -1.f, 1.f) * 90.f;
	Arc(Cx, Cy, RIn, Radius, -180, 180, 160, FLinearColor(0.06f, 0.06f, 0.07f, 0.95f), FLinearColor(0.16f, 0.16f, 0.18f, 0.95f));
	Arc(Cx, Cy, RIn - 2.f, RIn + 1.5f, -180, 180, 160, FLinearColor(0.35f, 0.35f, 0.38f, 0.9f), FLinearColor(0.35f, 0.35f, 0.38f, 0.9f));
	Arc(Cx, Cy, Radius - 2.f, Radius + 1.5f, -180, 180, 160, FLinearColor(0.02f, 0.02f, 0.02f, 0.9f), FLinearColor(0.02f, 0.02f, 0.02f, 0.9f));
	for (float Center : { -55.f, 55.f })
	{
		// suede grips with stitching where the hands go
		Arc(Cx, Cy, RIn + 1.5f, Radius - 2.f, Center - 22, Center + 22, 40, FLinearColor(0.11f, 0.11f, 0.13f, 1), FLinearColor(0.22f, 0.22f, 0.25f, 1));
		for (float Edge : { Center - 22.f, Center + 22.f })
		{
			Arc(Cx, Cy, RIn + 1.5f, Radius - 2.f, Edge - 0.6f, Edge + 0.6f, 2, FLinearColor(0.5f, 0.5f, 0.55f, 1), FLinearColor(0.5f, 0.5f, 0.55f, 1));
		}
	}
	// red 12 o'clock stripe
	Arc(Cx, Cy, RIn - 3.f, Radius + 3.f, -2.6f, 2.6f, 4, FLinearColor(0.85f, 0.05f, 0.05f, 1), FLinearColor(1.f, 0.15f, 0.12f, 1));
	WheelRotation = 0.f;
	const FString Status = bMouseSteering ? TEXT("MOUSE STEERING ON  -  click mouse wheel to release") : TEXT("Click the mouse wheel to steer with the mouse");
	Centered(Status, Base * 0.8f, bMouseSteering ? FLinearColor(0.3f, 1.f, 0.45f, 1) : FLinearColor(0.75f, 0.77f, 0.8f, 0.85f), VW / 2, Radius / 2.f + 3.f + 24.f);
}

// ------------------------------------------------------------------------------- results

void ARacingHUD::DrawResults(ARacingGameMode* Gm, const FRaceSnapshot& R)
{
	const FString MyName = Gm->GetPlayerCar()->Core().Name;
	const int32 Mine = R.Standings.IndexOfByPredicate([&MyName](const FRaceSnapshot::FRow& Row) { return Row.Name == MyName; });
	if (Mine == INDEX_NONE) return;
	const FRaceSnapshot::FRow& Me = R.Standings[Mine];
	const FRaceSnapshot::FRow& Winner = R.Standings[0];
	const float PanelW = 760, Ph = 560, PanelX = (VW - PanelW) / 2, Py = (VH - Ph) / 2;
	Rect(PanelX, Py, PanelW, Ph, FLinearColor(0.02f, 0.03f, 0.06f, 0.78f));
	for (int32 i = 0; i < 38; i++)
	{
		for (int32 j = 0; j < 2; j++) Rect(PanelX + i * 20, Py + Ph - 20 * (j + 1), 20, 20, (i + j) % 2 == 0 ? White : Black);
	}
	const bool bPenalized = Me.bFinished && !Me.bStopsMet;
	const FString Title = !Me.bFinished ? FString(TEXT("CHECKERED FLAG"))
		: bPenalized ? FString::Printf(TEXT("MISSED PIT STOP  -  CLASSIFIED P%d"), Me.Position)
		: FString::Printf(TEXT("CHECKERED FLAG  -  YOU FINISHED P%d"), Me.Position);
	Text(Title, Base * 2.4f, White, PanelX + 30, Py + Ph - 55, true);
	const float RowH = Base * 1.35f, RowTop = Py + Ph - 170;
	const float Cols[] = { 30, 100, 290, 365, 440, 615 };
	const TCHAR* Headers[] = { TEXT("POS"), TEXT("DRIVER"), TEXT("LAPS"), TEXT("PITS"), TEXT("TIME / GAP"), TEXT("BEST LAP") };
	for (int32 c = 0; c < 6; c++) Text(Headers[c], Base, FLinearColor(0.70f, 0.72f, 0.76f, 1), PanelX + Cols[c], RowTop + RowH);
	const TArray<int32> Shown = StandingsRows(R.Standings.Num(), Mine, 11);
	for (int32 r = 0; r < Shown.Num(); r++)
	{
		const float Y = RowTop - r * RowH;
		if (Shown[r] == INDEX_NONE)
		{
			Text(TEXT("..."), Base * 1.05f, White, PanelX + Cols[0], Y);
			continue;
		}
		const FRaceSnapshot::FRow& E = R.Standings[Shown[r]];
		if (Shown[r] == Mine) Rect(PanelX + 15, Y - RowH + 4 - 4, PanelW - 30, RowH, FLinearColor(0.10f, 0.32f, 0.85f, 0.8f));
		FString T;
		if (!E.bFinished) T = TEXT("running");
		else if (!E.bStopsMet) T = TEXT("PIT PENALTY");
		else if (Shown[r] == 0) T = FormatTime(E.FinishTime);
		else if (E.LapsCompleted < Winner.LapsCompleted) T = FString::Printf(TEXT("+%d lap"), Winner.LapsCompleted - E.LapsCompleted);
		else T = FString::Printf(TEXT("+%.3fs"), E.FinishTime - Winner.FinishTime);
		const float S = Base * 1.05f;
		Text(FString::Printf(TEXT("P%d"), E.Position), S, White, PanelX + Cols[0], Y);
		Text(E.bPlayer ? FString(TEXT("YOU")) : E.Name, S, White, PanelX + Cols[1], Y);
		Text(FString::FromInt(FMath::Min(E.LapsCompleted, R.RaceLaps)), S, White, PanelX + Cols[2], Y);
		Text(R.RequiredStops > 0 ? FString::Printf(TEXT("%d/%d"), E.PitStops, R.RequiredStops) : FString::FromInt(E.PitStops), S,
			E.bStopsMet ? White : FLinearColor(1.f, 0.35f, 0.3f, 1), PanelX + Cols[3], Y);
		Text(T, S, White, PanelX + Cols[4], Y);
		Text(FormatTime(E.BestLap), S, White, PanelX + Cols[5], Y);
	}
	Text(TEXT("ENTER / ESC: back to the menu"), Base, FLinearColor(0.70f, 0.72f, 0.76f, 1), PanelX + 30, Py + 45);
}

// ------------------------------------------------------------------------------- confirm

void ARacingHUD::DrawConfirm(ARacingGameMode* Gm)
{
	FString Title, Message, EnterText, EscText;
	Gm->GetConfirmText(Title, Message, EnterText, EscText);
	Rect(0, 0, VW, VH, FLinearColor(0, 0, 0, 0.6f));   // dims the screen
	const float Pw = 640, Ph = 250, Px = (VW - Pw) / 2, Py = (VH - Ph) / 2;
	Rect(Px, Py, Pw, Ph, FLinearColor(0.02f, 0.03f, 0.06f, 0.97f));
	Rect(Px, Py + Ph - 6, Pw, 6, FLinearColor(0.85f, 0.18f, 0.15f, 1));
	Centered(Title, Base * 2.0f, White, VW / 2, Py + Ph - 30, true);
	Centered(Message, Base, FLinearColor(0.75f, 0.77f, 0.80f, 1), VW / 2, Py + Ph - 90);
	Centered(TEXT("ENTER   ") + EnterText, Base * 1.15f, White, VW / 2, Py + 95);
	Centered(TEXT("ESC   ") + EscText, Base * 1.15f, White, VW / 2, Py + 60);
}
