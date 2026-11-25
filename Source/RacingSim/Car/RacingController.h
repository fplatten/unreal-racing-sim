#pragma once

#include "CoreMinimal.h"

/**
 * Racing wheel and pedals, the port of com.racingsim.app.RacingController: DirectInput wheels
 * (Logitech, Thrustmaster, Fanatec and most others) in its raw mode, and XInput devices (Xbox pads,
 * wheels in Xbox mode) in its gamepad mode, read directly rather than through Unreal's gamepad
 * input, whose 25% dead zone on the left stick axis swallowed a wheel's small corrections.
 *
 * XInput (gamepad mode): axes 0 LX, 1 LY, 2 RX, 3 RY, 4 LT, 5 RT; LX steers, RT is the throttle,
 * LT the brake; buttons 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start, 11-14 the D-pad.
 *
 * DirectInput (raw mode): the first connected DirectInput game controller is used, else the first
 * XInput one; it can be plugged in or out at any time. Axes are numbered as GLFW (and so the Java
 * game) numbered them: the device's axes in the order X, Y, Z, Rx, Ry, Rz, Slider 0, Slider 1,
 * skipping the ones it lacks. By default axis 0
 * steers, and the pedals are the other axes that rest at one end of their travel: the first is
 * the throttle, the second the brake (Logitech: Y and Rz). A pedal's resting end is taken from
 * the first reading, so pedals that run either way round work without setup.
 *
 * Wheels with force feedback get a centering spring that stiffens with speed, plus light damping.
 *
 * Anything can be overridden in an optional controller.properties next to RacingSim.exe (or in
 * the project folder), in the Java game's format, for example:
 *   device=G29              # the first controller whose name contains this text
 *   steer.axis=0            throttle.axis=Y        brake.axis=Rz     (number or axis name)
 *   steer.invert=false      steer.gain=1.0         steer.deadzone=0.01   pedal.deadzone=0.03
 *   button.shiftUp=4        button.shiftDown=5     button.camera=3       button.auto=2   (defaults)
 *   button.reset=6          button.pause=7         button.ok=0           button.back=1
 *   ffb=true                ffb.strength=0.5
 * The controller, its axes and the mapping in use are written to the log when it connects.
 */
class RACINGSIM_API FRacingController
{
public:
	enum class EButton : uint8 { ShiftUp, ShiftDown, Camera, Auto, Reset, Pause, Ok, Back, Up, Down, Left, Right, Count };

	static FRacingController& Get();

	/** Reads the wheel; call once per frame on the game thread. */
	void Poll(float Dt);
	/** Force feedback for the car being driven: speed (m/s); 0 when not driving. */
	void SetForceFeedback(float Speed);

	bool IsConnected() const { return bConnected; }
	const FString& GetName() const { return Name; }
	/** -1 (full right) .. +1 (full left), the sign of FVehicleInput::Steer. */
	float GetSteer() const { return Steer; }
	float GetThrottle() const { return Throttle; }
	float GetBrake() const { return Brake; }
	/** True on the frame the button went down. */
	bool WasPressed(EButton B) const { return Pressed[(int32)B]; }
	bool IsDown(EButton B) const { return Down[(int32)B]; }
	/** The device's own number of the last button pressed (-1: none yet), for setting up buttons. */
	int32 GetLastButton() const { return LastButton; }
	/** "steer X, throttle Y, brake Rz" for the menu. */
	FString Describe() const;

	~FRacingController();

private:
	FRacingController();
	void LoadConfig();
	bool Connect();
	bool ConnectXInput();
	void Disconnect();
	void SetupForceFeedback();
	float Pedal(float Raw, float Rest) const;

	struct FImpl;
	FImpl* Impl = nullptr;

	// configuration (controller.properties)
	FString DeviceFilter;
	FString SteerAxisCfg, ThrottleAxisCfg, BrakeAxisCfg;
	bool bSteerInvert = false;
	float SteerGain = 1.f, SteerDeadzone = 0.01f, PedalDeadzone = 0.03f;
	int32 ButtonCfg[(int32)EButton::Count];
	bool bFfbEnabled = true;
	float FfbStrength = 0.5f;

	// live state
	bool bConnected = false;
	FString Name;
	int32 SteerAxis = 0, ThrottleAxis = 1, BrakeAxis = 2;
	float PedalRest[2] = { 0.f, 0.f };
	int32 ButtonIndex[(int32)EButton::Count];
	bool Down[(int32)EButton::Count] = {};
	bool Pressed[(int32)EButton::Count] = {};
	float RescanTimer = 0.f;
	float Steer = 0.f, Throttle = 0.f, Brake = 0.f;
	int32 LastButton = -1;
	uint8 RawButtons[128] = {};
};
