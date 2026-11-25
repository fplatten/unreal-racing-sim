#include "Car/RacingController.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogRacingController, Log, All);

#if PLATFORM_WINDOWS
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericWindow.h"
#include "Widgets/SWindow.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <Xinput.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	/** Axis slots in GLFW's (and so the Java game's) order. */
	const TCHAR* AxisNames[8] = { TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("Rx"), TEXT("Ry"), TEXT("Rz"), TEXT("Slider0"), TEXT("Slider1") };
	/** XInput (gamepad mode) axes, GLFW gamepad numbering. */
	const TCHAR* PadAxisNames[6] = { TEXT("LX"), TEXT("LY"), TEXT("RX"), TEXT("RY"), TEXT("LT"), TEXT("RT") };
	const TCHAR* ButtonKeys[8] = { TEXT("shiftUp"), TEXT("shiftDown"), TEXT("camera"), TEXT("auto"), TEXT("reset"), TEXT("pause"), TEXT("ok"), TEXT("back") };
	/** Default buttons, the Java game's raw-mode set but with the shift paddles the Logitech way round
	 *  (right paddle 4 up, left 5 down); the D-pad comes from the hat switch. */
	const int32 DefaultButtons[(int32)FRacingController::EButton::Count] = { 4, 5, 3, 2, 6, 7, 0, 1, -1, -1, -1, -1 };
	/** XInput defaults (GLFW gamepad numbers, as the Java game): RB up, LB down, Y camera, X auto,
	 *  Back reset, Start pause, A ok, B back, D-pad up / down / left / right. */
	const int32 PadButtons[(int32)FRacingController::EButton::Count] = { 5, 4, 3, 2, 6, 7, 0, 1, 11, 13, 14, 12 };
}

#if PLATFORM_WINDOWS
struct FRacingController::FImpl
{
	IDirectInput8W* Di = nullptr;
	IDirectInputDevice8W* Device = nullptr;
	IDirectInputEffect* Spring = nullptr;
	IDirectInputEffect* Damper = nullptr;
	HWND Window = nullptr;
	bool bExclusive = false;
	int32 ExclusiveFailures = 0;
	/** Present axes: slot (0..7 in AxisNames) in order. */
	TArray<int32> Axes;
	DIJOYSTATE2 State = {};
	GUID Found = {};
	FString FoundName;
	FString Filter;
	bool bFound = false;

	// XInput (Xbox pads and wheels in Xbox mode), read directly: Unreal's own gamepad input has a
	// 25% dead zone on the left stick axis, which is what such a wheel steers with
	bool bXInput = false;
	DWORD XSlot = 0;
	float XAxes[6] = {};          // LX, LY, RX, RY, LT, RT, -1..1 (triggers rest at -1), as GLFW
	bool XButtons[15] = {};       // A B X Y LB RB Back Start Guide LThumb RThumb Up Right Down Left

	bool ReadXInput()
	{
		XINPUT_STATE St = {};
		if (XInputGetState(XSlot, &St) != ERROR_SUCCESS) return false;
		const XINPUT_GAMEPAD& G = St.Gamepad;
		auto Stick = [](SHORT V) { return FMath::Clamp((V + 0.5f) / 32767.5f, -1.f, 1.f); };
		XAxes[0] = Stick(G.sThumbLX);
		XAxes[1] = -Stick(G.sThumbLY);   // GLFW: down is positive
		XAxes[2] = Stick(G.sThumbRX);
		XAxes[3] = -Stick(G.sThumbRY);
		XAxes[4] = G.bLeftTrigger / 255.f * 2.f - 1.f;
		XAxes[5] = G.bRightTrigger / 255.f * 2.f - 1.f;
		const WORD Bits[15] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
			XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START, 0,
			XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_RIGHT,
			XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT };
		for (int32 i = 0; i < 15; i++) XButtons[i] = Bits[i] && (G.wButtons & Bits[i]) != 0;
		return true;
	}

	/** Axis by the mapping's number, whichever API the device is on. */
	float AxisNow(int32 Index) const
	{
		if (bXInput) return Index >= 0 && Index < 6 ? XAxes[Index] : 0.f;
		return Axis(Index);
	}

	bool ButtonNow(int32 Index) const
	{
		if (bXInput) return Index >= 0 && Index < 15 && XButtons[Index];
		return Index >= 0 && Index < 128 && (State.rgbButtons[Index] & 0x80) != 0;
	}

	static float AxisValue(const DIJOYSTATE2& S, int32 Slot)
	{
		LONG V = 0;
		switch (Slot)
		{
		case 0: V = S.lX; break;
		case 1: V = S.lY; break;
		case 2: V = S.lZ; break;
		case 3: V = S.lRx; break;
		case 4: V = S.lRy; break;
		case 5: V = S.lRz; break;
		case 6: V = S.rglSlider[0]; break;
		case 7: V = S.rglSlider[1]; break;
		}
		return FMath::Clamp((V + 0.5f) / 32767.5f, -1.f, 1.f);   // as GLFW
	}

	float Axis(int32 Index) const { return Axes.IsValidIndex(Index) ? AxisValue(State, Axes[Index]) : 0.f; }

	static bool IsXInput(IDirectInputDevice8W* Dev)
	{
		// XInput devices have "IG_" in their device path (the documented test; Windows reports the
		// path in lower case, "...&ig_00#...")
		DIPROPGUIDANDPATH P = {};
		P.diph.dwSize = sizeof(P);
		P.diph.dwHeaderSize = sizeof(DIPROPHEADER);
		P.diph.dwHow = DIPH_DEVICE;
		return SUCCEEDED(Dev->GetProperty(DIPROP_GUIDANDPATH, &P.diph)) && FString(P.wszPath).Contains(TEXT("IG_"), ESearchCase::IgnoreCase);
	}

	static BOOL CALLBACK OnDevice(const DIDEVICEINSTANCEW* Inst, void* Ctx)
	{
		FImpl* Self = static_cast<FImpl*>(Ctx);
		const FString DevName(Inst->tszProductName);
		if (!Self->Filter.IsEmpty() && !DevName.Contains(Self->Filter, ESearchCase::IgnoreCase)) return DIENUM_CONTINUE;
		IDirectInputDevice8W* Dev = nullptr;
		if (FAILED(Self->Di->CreateDevice(Inst->guidInstance, &Dev, nullptr))) return DIENUM_CONTINUE;
		const bool bXInput = IsXInput(Dev);
		Dev->Release();
		// Unreal reads XInput devices itself (-WheelAll: read them here too, for testing)
		static const bool bAll = FParse::Param(FCommandLine::Get(), TEXT("WheelAll"));
		if (bXInput && !bAll) return DIENUM_CONTINUE;
		Self->Found = Inst->guidInstance;
		Self->FoundName = DevName;
		Self->bFound = true;
		return DIENUM_STOP;
	}

	static BOOL CALLBACK OnAxis(const DIDEVICEOBJECTINSTANCEW* Obj, void* Ctx)
	{
		FImpl* Self = static_cast<FImpl*>(Ctx);
		int32 Slot = -1;
		if (Obj->guidType == GUID_XAxis) Slot = 0;
		else if (Obj->guidType == GUID_YAxis) Slot = 1;
		else if (Obj->guidType == GUID_ZAxis) Slot = 2;
		else if (Obj->guidType == GUID_RxAxis) Slot = 3;
		else if (Obj->guidType == GUID_RyAxis) Slot = 4;
		else if (Obj->guidType == GUID_RzAxis) Slot = 5;
		else if (Obj->guidType == GUID_Slider) Slot = Self->Axes.Contains(6) ? 7 : 6;
		if (Slot >= 0) Self->Axes.AddUnique(Slot);
		return DIENUM_CONTINUE;
	}

	HWND GameWindow() const
	{
		if (GEngine && GEngine->GameViewport)
		{
			const TSharedPtr<SWindow> W = GEngine->GameViewport->GetWindow();
			if (W.IsValid() && W->GetNativeWindow().IsValid()) return (HWND)W->GetNativeWindow()->GetOSWindowHandle();
		}
		return nullptr;
	}

	void ReleaseEffects()
	{
		if (Spring) { Spring->Stop(); Spring->Release(); Spring = nullptr; }
		if (Damper) { Damper->Stop(); Damper->Release(); Damper = nullptr; }
	}

	void ReleaseDevice()
	{
		ReleaseEffects();
		if (Device) { Device->Unacquire(); Device->Release(); Device = nullptr; }
		Axes.Reset();
	}

	/** One condition effect (spring or damper) on the steering axis. */
	IDirectInputEffect* MakeCondition(const GUID& Type, LONG Coefficient)
	{
		DWORD AxisOfs[1] = { DIJOFS_X };
		LONG Dir[1] = { 0 };
		DICONDITION Cond = {};
		Cond.lPositiveCoefficient = Cond.lNegativeCoefficient = Coefficient;
		Cond.dwPositiveSaturation = Cond.dwNegativeSaturation = DI_FFNOMINALMAX;
		DIEFFECT Eff = {};
		Eff.dwSize = sizeof(DIEFFECT);
		Eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
		Eff.dwDuration = INFINITE;
		Eff.dwGain = DI_FFNOMINALMAX;
		Eff.dwTriggerButton = DIEB_NOTRIGGER;
		Eff.cAxes = 1;
		Eff.rgdwAxes = AxisOfs;
		Eff.rglDirection = Dir;
		Eff.cbTypeSpecificParams = sizeof(DICONDITION);
		Eff.lpvTypeSpecificParams = &Cond;
		IDirectInputEffect* E = nullptr;
		if (FAILED(Device->CreateEffect(Type, &Eff, &E, nullptr))) return nullptr;
		E->Start(1, 0);
		return E;
	}

	static void SetCoefficient(IDirectInputEffect* E, LONG Coefficient)
	{
		if (!E) return;
		DICONDITION Cond = {};
		Cond.lPositiveCoefficient = Cond.lNegativeCoefficient = Coefficient;
		Cond.dwPositiveSaturation = Cond.dwNegativeSaturation = DI_FFNOMINALMAX;
		DIEFFECT Eff = {};
		Eff.dwSize = sizeof(DIEFFECT);
		Eff.cbTypeSpecificParams = sizeof(DICONDITION);
		Eff.lpvTypeSpecificParams = &Cond;
		E->SetParameters(&Eff, DIEP_TYPESPECIFICPARAMS);
	}
};
#else
struct FRacingController::FImpl {};
#endif

FRacingController& FRacingController::Get()
{
	static FRacingController Instance;
	return Instance;
}

FRacingController::FRacingController()
{
	for (int32& B : ButtonCfg) B = -1;
	for (int32 i = 0; i < (int32)EButton::Count; i++) ButtonIndex[i] = DefaultButtons[i];
	LoadConfig();
#if PLATFORM_WINDOWS
	Impl = new FImpl();
	Impl->Filter = DeviceFilter;
	if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&Impl->Di, nullptr)))
	{
		UE_LOG(LogRacingController, Warning, TEXT("DirectInput is not available: racing wheels other than XInput ones will not work"));
		Impl->Di = nullptr;
	}
#endif
}

FRacingController::~FRacingController()
{
#if PLATFORM_WINDOWS
	if (Impl)
	{
		Impl->ReleaseDevice();
		if (Impl->Di) Impl->Di->Release();
		delete Impl;
	}
#endif
}

void FRacingController::LoadConfig()
{
	// controller.properties next to RacingSim.exe, else in the project folder
	FString Text;
	FString File;
	for (const FString& Dir : { FPaths::RootDir(), FPaths::LaunchDir(), FPaths::ProjectDir() })
	{
		File = FPaths::Combine(Dir, TEXT("controller.properties"));
		if (FFileHelper::LoadFileToString(Text, *File)) break;
		Text.Reset();
	}
	if (Text.IsEmpty()) return;
	UE_LOG(LogRacingController, Display, TEXT("Controller settings read from %s"), *FPaths::ConvertRelativePathToFull(File));
	TMap<FString, FString> P;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (FString Line : Lines)
	{
		int32 Hash;
		if (Line.FindChar(TEXT('#'), Hash)) Line.LeftInline(Hash);
		FString Key, Value;
		if (Line.Split(TEXT("="), &Key, &Value)) P.Add(Key.TrimStartAndEnd().ToLower(), Value.TrimStartAndEnd());
	}
	auto Float = [&P](const TCHAR* K, float Def) { const FString* V = P.Find(K); return V && V->IsNumeric() ? FCString::Atof(**V) : Def; };
	auto Bool = [&P](const TCHAR* K, bool Def) { const FString* V = P.Find(K); return V ? V->ToBool() : Def; };
	if (const FString* V = P.Find(TEXT("device"))) DeviceFilter = *V;
	if (const FString* V = P.Find(TEXT("steer.axis"))) SteerAxisCfg = *V;
	if (const FString* V = P.Find(TEXT("throttle.axis"))) ThrottleAxisCfg = *V;
	if (const FString* V = P.Find(TEXT("brake.axis"))) BrakeAxisCfg = *V;
	bSteerInvert = Bool(TEXT("steer.invert"), false);
	SteerGain = Float(TEXT("steer.gain"), 1.f);
	SteerDeadzone = Float(TEXT("steer.deadzone"), 0.01f);
	PedalDeadzone = Float(TEXT("pedal.deadzone"), 0.03f);
	bFfbEnabled = Bool(TEXT("ffb"), true);
	FfbStrength = FMath::Clamp(Float(TEXT("ffb.strength"), 0.5f), 0.f, 1.f);
	for (int32 i = 0; i < UE_ARRAY_COUNT(ButtonKeys); i++)
	{
		if (const FString* V = P.Find(FString(TEXT("button.")) + FString(ButtonKeys[i]).ToLower())) if (V->IsNumeric()) ButtonCfg[i] = FCString::Atoi(**V);
	}
}

float FRacingController::Pedal(float Raw, float Rest) const
{
	// travel 0..1 from the resting end (+1, 0 or -1), with a dead zone at both ends
	const float Span = Rest == 0.f ? 1.f : 2.f;
	float V = FMath::Abs(Raw - Rest) / Span;
	V = (V - PedalDeadzone) / (1.f - 2.f * PedalDeadzone);
	return FMath::Clamp(V, 0.f, 1.f);
}

bool FRacingController::Connect()
{
#if PLATFORM_WINDOWS
	FImpl& I = *Impl;
	// a DirectInput wheel first; else an XInput device (an Xbox pad, or a wheel in Xbox mode)
	if (!I.Di) return ConnectXInput();
	I.bFound = false;
	I.Di->EnumDevices(DI8DEVCLASS_GAMECTRL, &FImpl::OnDevice, &I, DIEDFL_ATTACHEDONLY);
	if (!I.bFound || FAILED(I.Di->CreateDevice(I.Found, &I.Device, nullptr))) return ConnectXInput();
	if (FAILED(I.Device->SetDataFormat(&c_dfDIJoystick2)))
	{
		I.ReleaseDevice();
		return false;
	}
	I.Window = I.GameWindow();
	DIDEVCAPS Caps = {};
	Caps.dwSize = sizeof(Caps);
	I.Device->GetCapabilities(&Caps);
	const bool bFfb = bFfbEnabled && (Caps.dwFlags & DIDC_FORCEFEEDBACK) && I.Window && I.ExclusiveFailures < 3;
	// force feedback needs the device exclusively (while our window has focus); otherwise share it
	I.bExclusive = bFfb && SUCCEEDED(I.Device->SetCooperativeLevel(I.Window, DISCL_EXCLUSIVE | DISCL_FOREGROUND));
	if (!I.bExclusive) I.Device->SetCooperativeLevel(I.Window ? I.Window : GetDesktopWindow(), DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);

	// axes: range -32768..32767 like GLFW, in GLFW's order
	DIPROPRANGE Range = {};
	Range.diph.dwSize = sizeof(Range);
	Range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	Range.diph.dwHow = DIPH_DEVICE;
	Range.lMin = -32768;
	Range.lMax = 32767;
	I.Device->SetProperty(DIPROP_RANGE, &Range.diph);
	I.Axes.Reset();
	I.Device->EnumObjects(&FImpl::OnAxis, &I, DIDFT_AXIS);
	I.Axes.Sort();
	I.Device->Acquire();
	I.Device->Poll();
	if (FAILED(I.Device->GetDeviceState(sizeof(DIJOYSTATE2), &I.State)))
	{
		FMemory::Memzero(I.State);
		I.State.lX = I.State.lY = I.State.lZ = I.State.lRx = I.State.lRy = I.State.lRz = 0;
	}
	Name = I.FoundName;

	// axis mapping: configured (number or name), else steering on axis 0 and the pedals on the
	// first two other axes resting at one end of their travel
	auto Resolve = [&I](const FString& Cfg) -> int32
	{
		if (Cfg.IsEmpty()) return INDEX_NONE;
		if (Cfg.IsNumeric()) return FCString::Atoi(*Cfg);
		for (int32 Slot = 0; Slot < 8; Slot++)
		{
			if (Cfg.Equals(AxisNames[Slot], ESearchCase::IgnoreCase) || (Slot == 6 && Cfg.Equals(TEXT("Slider"), ESearchCase::IgnoreCase)))
			{
				return I.Axes.IndexOfByKey(Slot);
			}
		}
		return INDEX_NONE;
	};
	SteerAxis = Resolve(SteerAxisCfg);
	if (SteerAxis == INDEX_NONE) SteerAxis = 0;
	TArray<int32> AtRest;
	for (int32 i = 0; i < I.Axes.Num(); i++)
	{
		if (i != SteerAxis && FMath::Abs(I.Axis(i)) > 0.9f) AtRest.Add(i);
	}
	ThrottleAxis = Resolve(ThrottleAxisCfg);
	if (ThrottleAxis == INDEX_NONE) ThrottleAxis = AtRest.Num() >= 2 ? AtRest[0] : 1;
	BrakeAxis = Resolve(BrakeAxisCfg);
	if (BrakeAxis == INDEX_NONE) BrakeAxis = AtRest.Num() >= 2 ? AtRest[1] : 2;
	PedalRest[0] = FMath::RoundToFloat(I.Axis(ThrottleAxis));
	PedalRest[1] = FMath::RoundToFloat(I.Axis(BrakeAxis));
	for (int32 b = 0; b < (int32)EButton::Count; b++) ButtonIndex[b] = ButtonCfg[b] >= 0 ? ButtonCfg[b] : DefaultButtons[b];
	FMemory::Memzero(Down);
	FMemory::Memzero(Pressed);
	bConnected = true;

	if (I.bExclusive) SetupForceFeedback();
	FString Raw;
	for (int32 i = 0; i < I.Axes.Num(); i++) Raw += FString::Printf(TEXT(" %d:%s=%+.2f"), i, AxisNames[I.Axes[i]], I.Axis(i));
	UE_LOG(LogRacingController, Display, TEXT("Racing wheel connected: '%s' (DirectInput). %s. Pedals rest at %+.0f / %+.0f. Axes:%s. %d buttons. Force feedback: %s"),
		*Name, *Describe(), PedalRest[0], PedalRest[1], *Raw, (int32)Caps.dwButtons, I.Spring ? TEXT("on") : bFfb ? TEXT("not available") : TEXT("off"));
	return true;
#else
	return false;
#endif
}

bool FRacingController::ConnectXInput()
{
#if PLATFORM_WINDOWS
	FImpl& I = *Impl;
	for (DWORD Slot = 0; Slot < XUSER_MAX_COUNT; Slot++)
	{
		I.XSlot = Slot;
		if (!I.ReadXInput()) continue;
		XINPUT_CAPABILITIES Caps = {};
		const bool bWheel = XInputGetCapabilities(Slot, 0, &Caps) == ERROR_SUCCESS && Caps.SubType == XINPUT_DEVSUBTYPE_WHEEL;
		Name = FString::Printf(TEXT("%s (Xbox mode, player %d)"), bWheel ? TEXT("Racing wheel") : TEXT("Controller"), (int32)Slot + 1);
		if (!DeviceFilter.IsEmpty() && !Name.Contains(DeviceFilter, ESearchCase::IgnoreCase)) continue;
		I.bXInput = true;
		// GLFW gamepad mapping (as the Java game): left stick X steers, right trigger is the
		// throttle, left trigger the brake; numbers or names (LX LY RX RY LT RT) in the settings
		auto Resolve = [](const FString& Cfg, int32 Default) -> int32
		{
			if (Cfg.IsEmpty()) return Default;
			if (Cfg.IsNumeric()) return FCString::Atoi(*Cfg);
			for (int32 i = 0; i < 6; i++) if (Cfg.Equals(PadAxisNames[i], ESearchCase::IgnoreCase)) return i;
			return Default;
		};
		SteerAxis = Resolve(SteerAxisCfg, 0);
		ThrottleAxis = Resolve(ThrottleAxisCfg, 5);
		BrakeAxis = Resolve(BrakeAxisCfg, 4);
		// triggers rest at -1, sticks at 0
		PedalRest[0] = ThrottleAxis >= 4 ? -1.f : 0.f;
		PedalRest[1] = BrakeAxis >= 4 ? -1.f : 0.f;
		for (int32 b = 0; b < (int32)EButton::Count; b++) ButtonIndex[b] = ButtonCfg[b] >= 0 ? ButtonCfg[b] : PadButtons[b];
		FMemory::Memzero(Down);
		FMemory::Memzero(Pressed);
		bConnected = true;
		UE_LOG(LogRacingController, Display, TEXT("Racing controller connected: '%s' (XInput, read directly, steering dead zone %.0f%%). %s"),
			*Name, SteerDeadzone * 100.f, *Describe());
		return true;
	}
	I.bXInput = false;
#endif
	return false;
}

void FRacingController::SetupForceFeedback()
{
#if PLATFORM_WINDOWS
	FImpl& I = *Impl;
	// the wheel's own centering spring off: ours follows the car's speed
	DIPROPDWORD Auto = {};
	Auto.diph.dwSize = sizeof(Auto);
	Auto.diph.dwHeaderSize = sizeof(DIPROPHEADER);
	Auto.diph.dwHow = DIPH_DEVICE;
	Auto.dwData = DIPROPAUTOCENTER_OFF;
	I.Device->Unacquire();
	I.Device->SetProperty(DIPROP_AUTOCENTER, &Auto.diph);
	if (FAILED(I.Device->Acquire())) return;   // not in the foreground yet: Poll() tries again
	I.Spring = I.MakeCondition(GUID_Spring, 0);
	I.Damper = I.MakeCondition(GUID_Damper, 0);
#endif
}

void FRacingController::Disconnect()
{
	UE_LOG(LogRacingController, Display, TEXT("Racing wheel disconnected: '%s'"), *Name);
#if PLATFORM_WINDOWS
	Impl->ReleaseDevice();
	Impl->bXInput = false;
#endif
	bConnected = false;
	Steer = Throttle = Brake = 0.f;
	FMemory::Memzero(Down);
	FMemory::Memzero(Pressed);
	RescanTimer = 0.f;
}

void FRacingController::Poll(float Dt)
{
	FMemory::Memzero(Pressed);
#if PLATFORM_WINDOWS
	if (!Impl) return;
	FImpl& I = *Impl;
	if (!bConnected)
	{
		RescanTimer -= Dt;
		if (RescanTimer > 0.f) return;
		RescanTimer = 1.f;   // look for a wheel once a second
		if (!Connect()) return;
	}
	if (I.bXInput)
	{
		if (!I.ReadXInput())
		{
			Disconnect();
			return;
		}
	}
	else
	{
		HRESULT Hr = I.Device->Poll();
		if (FAILED(Hr))
		{
			Hr = I.Device->Acquire();
			// exclusive (force feedback) access is only granted while our window has focus
			if (Hr == DIERR_OTHERAPPHASPRIO || Hr == DIERR_NOTACQUIRED) return;
			if (Hr == DIERR_INPUTLOST || Hr == DIERR_UNPLUGGED || FAILED(Hr))
			{
				Disconnect();
				return;
			}
			if (I.bExclusive && !I.Spring) SetupForceFeedback();
			I.Device->Poll();
		}
		Hr = I.Device->GetDeviceState(sizeof(DIJOYSTATE2), &I.State);
		if (Hr == DIERR_INPUTLOST || Hr == DIERR_NOTACQUIRED)
		{
			I.Device->Acquire();
			return;
		}
		if (FAILED(Hr))
		{
			Disconnect();
			return;
		}
	}

	// DirectInput and XInput (GLFW numbering): right is positive; the car: left is positive
	float S = -I.AxisNow(SteerAxis) * (bSteerInvert ? -1.f : 1.f);
	S = FMath::Abs(S) < SteerDeadzone ? 0.f : FMath::Sign(S) * (FMath::Abs(S) - SteerDeadzone) / (1.f - SteerDeadzone);
	Steer = FMath::Clamp(S * SteerGain, -1.f, 1.f);
	Throttle = Pedal(I.AxisNow(ThrottleAxis), PedalRest[0]);
	Brake = Pedal(I.AxisNow(BrakeAxis), PedalRest[1]);

	// any button going down: remembered for the menu, so the buttons can be set up
	const int32 NumButtons = I.bXInput ? 15 : 128;
	for (int32 i = 0; i < NumButtons; i++)
	{
		const uint8 B = I.ButtonNow(i) ? 1 : 0;
		if (B && !RawButtons[i]) LastButton = i;
		RawButtons[i] = B;
	}
	bool Now[(int32)EButton::Count] = {};
	for (int32 b = 0; b < (int32)EButton::Count; b++) Now[b] = I.ButtonNow(ButtonIndex[b]);
	// first hat switch = D-pad (hundredths of a degree clockwise from up; centered is 0xFFFF)
	const DWORD Pov = I.bXInput ? 0xFFFF : I.State.rgdwPOV[0];
	if (LOWORD(Pov) != 0xFFFF)
	{
		const int32 Deg = Pov / 100;
		Now[(int32)EButton::Up] |= Deg >= 315 || Deg <= 45;
		Now[(int32)EButton::Right] |= Deg >= 45 && Deg <= 135;
		Now[(int32)EButton::Down] |= Deg >= 135 && Deg <= 225;
		Now[(int32)EButton::Left] |= Deg >= 225 && Deg <= 315;
	}
	for (int32 b = 0; b < (int32)EButton::Count; b++)
	{
		Pressed[b] = Now[b] && !Down[b];
		Down[b] = Now[b];
	}
#endif
}

void FRacingController::SetForceFeedback(float Speed)
{
#if PLATFORM_WINDOWS
	if (!bConnected || !Impl || !Impl->Spring) return;
	// centering spring: light when parked, firm at speed; light damping for weight
	const float V = FMath::Clamp(FMath::Abs(Speed) / 90.f, 0.f, 1.f);
	const LONG Spring = (LONG)(DI_FFNOMINALMAX * FfbStrength * (0.15f + 0.85f * V));
	const LONG Damper = (LONG)(DI_FFNOMINALMAX * FfbStrength * 0.25f);
	FImpl::SetCoefficient(Impl->Spring, Spring);
	FImpl::SetCoefficient(Impl->Damper, Damper);
#endif
}

FString FRacingController::Describe() const
{
#if PLATFORM_WINDOWS
	auto AxisName = [this](int32 Index) -> FString
	{
		if (Impl && Impl->bXInput) return Index >= 0 && Index < 6 ? FString(PadAxisNames[Index]) : FString::Printf(TEXT("axis %d (missing)"), Index);
		return Impl && Impl->Axes.IsValidIndex(Index) ? FString(AxisNames[Impl->Axes[Index]]) : FString::Printf(TEXT("axis %d (missing)"), Index);
	};
	return FString::Printf(TEXT("steer %s, throttle %s, brake %s"), *AxisName(SteerAxis), *AxisName(ThrottleAxis), *AxisName(BrakeAxis));
#else
	return FString();
#endif
}
