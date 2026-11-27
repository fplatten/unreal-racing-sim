#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Containers/Queue.h"
#include "Sim/RaceLogic.h"
#include <atomic>

class FSocket;
class FRunnableThread;

/**
 * External bots (port of com.racingsim.bot): a bot is another program that drives one car over a
 * local TCP connection, one JSON object per line. The protocol is the Java version's
 * (racing-sim/bots/PROTOCOL.md), so a bot written for one game races in both.
 */
namespace BotProtocol
{
	constexpr int32 Protocol = 1;
	constexpr int32 DefaultPort = 4510;
	constexpr int32 PhysicsHz = 240;
	constexpr int32 MaxLine = 256 * 1024;

	/** Compact JSON writer (port of Json.Out): numbers with 6 significant digits, NaN as null. */
	class FOut
	{
	public:
		FOut& Obj() { Sep(); B.AppendChar('{'); bComma = false; return *this; }
		FOut& Arr() { Sep(); B.AppendChar('['); bComma = false; return *this; }
		FOut& EndObj() { B.AppendChar('}'); bComma = true; return *this; }
		FOut& EndArr() { B.AppendChar(']'); bComma = true; return *this; }
		FOut& Key(const TCHAR* K);
		FOut& Num(double V);
		FOut& Num(const TCHAR* K, double V) { return Key(K).Num(V); }
		FOut& Str(const FString& V);
		FOut& Str(const TCHAR* K, const FString& V) { return Key(K).Str(V); }
		FOut& Bool(const TCHAR* K, bool V);
		const FString& ToString() const { return B; }

	private:
		void Sep() { if (bComma) B.AppendChar(','); bComma = true; }
		void Quote(const FString& V);
		FString B;
		bool bComma = false;
	};

	/** What the race is doing, for the state's race.phase. */
	enum class EPhase : uint8 { Grid, Lights, Green, Finished };

	/** The once-per-race track description (columns s,x,y,z,heading,curvature,minD,maxD,bank,grade). */
	FString TrackMessage(const FTrackSpline& Track);

	/** The car's heading relative to the track (rad, + = pointing right of the track direction). */
	float HeadingError(const FCarCore& Car, const FTrackSpline& Track);

	/** Distance to the first wall along a straight line at Angle to the track direction (+ right),
	 *  marched in track coordinates (BotSensors.ray). */
	float Ray(const FTrackSpline& Track, float S, float D, float Angle);

	/** One control tick's state message. CarIds: the protocol id of each car of Field (1-based). */
	FString StateMessage(int64 Tick, float Time, const FCarCore& Car, const FTrackSpline& Track,
		const TArray<FCarCore*>& Field, const FLapTracker& Laps, const FLapTracker::FEntry& Entry,
		EPhase Phase, int32 LightsOn);
}

/** One connected bot. The server's thread reads and writes its socket; everything else goes
 *  through this object's locks and queue, so the physics step never waits on the network. */
class RACINGSIM_API FBotConnection
{
public:
	/** A bot's driving command, already clamped. Steering here is the game's: + = left. */
	struct FControl
	{
		int64 Tick = -1;
		float Steer = 0.f, Throttle = 0.f, Brake = 0.f;
		int32 Shift = 0;
		/** Debug lines (s1, d1, s2, d2) and their colors. */
		TArray<FVector4f> Lines;
		TArray<FColor> LineColors;
	};

	FBotConnection(FSocket* InSocket, int32 InId);
	~FBotConnection();

	int32 Id() const { return ConnId; }
	FString Name() const { FScopeLock L(&Lock); return BotName; }
	bool IsOpen() const { return bOpen.load(); }
	bool HelloReceived() const { return bHello.load(); }
	bool Latest(FControl& Out) const;
	/** Queues a line for sending (any thread). */
	void Send(const FString& Line);
	void Close() { bOpen = false; }

	/** Server thread: reads what has arrived and sends what is queued. False once closed. */
	bool Pump();

private:
	void Handle(const FString& Line);

	FSocket* Socket;
	int32 ConnId;
	std::atomic<bool> bOpen{ true };
	std::atomic<bool> bHello{ false };
	mutable FCriticalSection Lock;
	FString BotName = TEXT("Bot");
	FControl Control;
	TQueue<FString, EQueueMode::Mpsc> Outbox;
	TArray<uint8> Inbox;
	int32 ProtocolErrors = 0;
};

/** Listens on 127.0.0.1 (-Bots=<port>) and keeps the bots that have said hello. */
class RACINGSIM_API FBotServer : public FRunnable
{
public:
	/** The port given with -Bots=<port> (or -Bots alone: 4510), -1 for none. */
	static int32 ConfiguredPort();
	/** Null if the port cannot be opened. */
	static TUniquePtr<FBotServer> Start(int32 Port);
	virtual ~FBotServer() override;

	int32 GetPort() const { return Port; }
	/** Connected bots that have said hello, in the order they connected. */
	TArray<TSharedPtr<FBotConnection>> Ready() const;

	virtual uint32 Run() override;
	virtual void Stop() override { bStop = true; }

private:
	FBotServer() = default;
	FSocket* Listener = nullptr;
	FRunnableThread* Thread = nullptr;
	int32 Port = 0;
	std::atomic<bool> bStop{ false };
	mutable FCriticalSection Lock;
	TArray<TSharedPtr<FBotConnection>> Bots;
	int32 NextId = 1;
};

/**
 * Drives a car from a bot's commands (port of ExternalDriver), realtime only: every
 * 240 / ControlHz physics steps the bot gets a state and between those its newest command holds;
 * a bot that answers nothing for 2 s, or goes away, is lost and the car goes back to its AI driver
 * (or to the player's controls).
 */
class RACINGSIM_API FExternalDriver
{
public:
	FExternalDriver(TSharedPtr<FBotConnection> InConn, FCarCore* InCar, int32 InControlHz);

	FBotConnection& Connection() const { return *Conn; }
	FCarCore* Car() const { return CarCore; }
	bool IsLost() const { return bLost.load(); }
	/** The newest debug lines (game thread, for drawing). */
	void DebugLines(TArray<FVector4f>& Lines, TArray<FColor>& Colors) const;

	/** Sends welcome and track at the start of a race. */
	void Welcome(int32 CarId, const FString& Mode, const FTrackSpline& Track);
	/** One physics step; bDrive false while the field waits on the grid (the director holds the brakes). */
	void Update(float Dt, const FTrackSpline& Track, const TArray<FCarCore*>& Field, const FLapTracker& Laps,
		const FLapTracker::FEntry& Entry, BotProtocol::EPhase Phase, int32 LightsOn, bool bDrive);
	/** Sends an event (lapComplete, contact, recovered): Fields is a JSON object's members, "" for none. */
	void Event(const TCHAR* Kind, const FString& Fields);

private:
	void Lose(const TCHAR* Why);

	TSharedPtr<FBotConnection> Conn;
	FCarCore* CarCore;
	int32 ControlHz, StepsPerControl;
	int64 Tick = 0;
	int64 Steps = 0;
	int64 AppliedShiftTick = -1;
	std::atomic<bool> bLost{ false };
	FBotConnection::FControl Held;
	bool bHasHeld = false;
	mutable FCriticalSection DebugLock;
	TArray<FVector4f> DebugLineData;
	TArray<FColor> DebugColors;
};
