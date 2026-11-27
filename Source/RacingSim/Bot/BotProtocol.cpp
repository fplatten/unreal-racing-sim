#include "Bot/BotProtocol.h"

#include "Sim/VehicleSim.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/RunnableThread.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

// ---------------------------------------------------------------- JSON writer

namespace BotProtocol
{
	FOut& FOut::Key(const TCHAR* K)
	{
		Sep();
		Quote(K);
		B.AppendChar(':');
		bComma = false;
		return *this;
	}

	FOut& FOut::Num(double V)
	{
		Sep();
		if (FMath::IsNaN(V) || !FMath::IsFinite(V)) { B += TEXT("null"); return *this; }
		if (V == FMath::RoundToDouble(V) && FMath::Abs(V) < 1e15) { B += FString::Printf(TEXT("%lld"), (long long)V); return *this; }
		FString S = FString::Printf(TEXT("%.6g"), V);
		// trim trailing zeros of the fraction ("1.500000" -> "1.5"), keeping any exponent
		int32 E = INDEX_NONE;
		S.FindChar('e', E);
		FString Mant = E == INDEX_NONE ? S : S.Left(E), Exp = E == INDEX_NONE ? FString() : S.Mid(E);
		if (Mant.Contains(TEXT(".")))
		{
			while (Mant.EndsWith(TEXT("0"))) Mant.LeftChopInline(1);
			if (Mant.EndsWith(TEXT("."))) Mant.LeftChopInline(1);
		}
		B += Mant + Exp;
		return *this;
	}

	FOut& FOut::Str(const FString& V)
	{
		Sep();
		Quote(V);
		return *this;
	}

	FOut& FOut::Bool(const TCHAR* K, bool V)
	{
		Key(K);
		Sep();
		B += V ? TEXT("true") : TEXT("false");
		return *this;
	}

	void FOut::Quote(const FString& V)
	{
		B.AppendChar('"');
		for (TCHAR C : V)
		{
			switch (C)
			{
			case '"': B += TEXT("\\\""); break;
			case '\\': B += TEXT("\\\\"); break;
			case '\n': B += TEXT("\\n"); break;
			case '\r': B += TEXT("\\r"); break;
			case '\t': B += TEXT("\\t"); break;
			default:
				if (C < 0x20) B += FString::Printf(TEXT("\\u%04x"), (int32)C);
				else B.AppendChar(C);
			}
		}
		B.AppendChar('"');
	}

	static double Round(double V, double Scale) { return FMath::RoundToDouble(V * Scale) / Scale; }

	// ---------------------------------------------------------------- sensors (BotSensors)

	static constexpr float Ahead[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300 };
	static constexpr int32 Rays = 19;
	static constexpr float RaySpreadDeg = 90.f, RayMax = 200.f, RayStep = 1.f;
	static constexpr int32 Opponents = 8;
	static constexpr float OpponentRange = 200.f;
	static constexpr float TrackStep = 5.f;

	FString TrackMessage(const FTrackSpline& Track)
	{
		FOut O;
		O.Obj().Str(TEXT("type"), TEXT("track")).Str(TEXT("layout"), FTrackSpline::LayoutKey(FTrackSpline::Layout))
			.Num(TEXT("length"), FTrackSpline::Length).Num(TEXT("step"), TrackStep)
			.Str(TEXT("columns"), TEXT("s,x,y,z,heading,curvature,minD,maxD,bank,grade")).Key(TEXT("samples")).Arr();
		for (float S = 0.f; S < FTrackSpline::Length - 0.01f; S += TrackStep)
		{
			const FVector P = Track.Point(S, 0.f);
			// the protocol's world frame has y up: Unreal's z
			O.Arr().Num(S).Num(Round(P.X, 100)).Num(Round(P.Z, 100)).Num(Round(P.Y, 100))
				.Num(Round(Track.Heading(S), 1e5)).Num(Round(Track.Curvature(S), 1e6))
				.Num(Round(Track.MinD(S), 100)).Num(Round(Track.MaxD(S), 100))
				.Num(Round(Track.Bank(S), 1e4)).Num(Round(Track.Grade(S), 1e4)).EndArr();
		}
		O.EndArr().EndObj();
		return O.ToString();
	}

	float HeadingError(const FCarCore& Car, const FTrackSpline& Track)
	{
		const FVector F = Car.Body.Rot.GetForwardVector();
		const FVector TF = Track.Forward(Car.S), TR = Track.Right(Car.S);
		return FMath::Atan2(FVector::DotProduct(F, TR), FVector::DotProduct(F, TF));
	}

	float Ray(const FTrackSpline& Track, float S, float D, float Angle)
	{
		float Theta = Angle;
		for (float R = 0.f; R < RayMax; R += RayStep)
		{
			const float K = Track.Curvature(S);
			const float Scale = 1.f + K * D;
			const float Ds = RayStep * FMath::Cos(Theta) / FMath::Max(Scale, 0.2f);
			S += Ds;
			D += RayStep * FMath::Sin(Theta);
			Theta += K * Ds;
			if (D < Track.MinD(S) || D > Track.MaxD(S)) return R + RayStep;
		}
		return RayMax;
	}

	FString StateMessage(int64 Tick, float Time, const FCarCore& Car, const FTrackSpline& Track,
		const TArray<FCarCore*>& Field, const FLapTracker& Laps, const FLapTracker::FEntry& Entry,
		EPhase Phase, int32 LightsOn)
	{
		const FVehicleSim& Ph = *Car.Sim;
		const float S = Car.S, D = Car.D;
		const float Herr = HeadingError(Car, Track);
		const FVector Up = Car.Body.Rot.GetUpVector();
		const FVector F = Car.Body.Rot.GetForwardVector();
		// Unreal is left-handed: up x forward points right
		const FVector Right = FVector::CrossProduct(Up, F).GetSafeNormal();
		int32 OnTrack = 0;
		for (int32 W = 0; W < 4; W++) if (Ph.GetWheels()[W].bContact && Ph.GetWheels()[W].bOnTrack) OnTrack++;
		const FVehicleSpec& Spec = Ph.GetSpec();

		FOut O;
		O.Obj().Str(TEXT("type"), TEXT("state")).Num(TEXT("tick"), (double)Tick).Num(TEXT("t"), Round(Time, 1000));
		O.Key(TEXT("car")).Obj()
			.Num(TEXT("s"), Round(S, 1000)).Num(TEXT("d"), Round(D, 1000))
			.Num(TEXT("speed"), Round(Ph.GetSpeed(), 1000)).Num(TEXT("vLong"), Round(Ph.GetForwardSpeed(), 1000))
			.Num(TEXT("vLat"), Round(FVector::DotProduct(Car.Body.Vel, Right), 1000)).Num(TEXT("dRate"), Round(Car.LateralRate, 1000))
			// a positive angular velocity about up turns +X toward +Y: to the right
			.Num(TEXT("yawRate"), Round(FVector::DotProduct(Car.Body.AngVel, Up), 1e4)).Num(TEXT("headingErr"), Round(Herr, 1e4))
			.Num(TEXT("rpm"), FMath::RoundToDouble(Ph.GetEngineRpm())).Num(TEXT("gear"), Ph.GetGearbox().GetGear())
			.Num(TEXT("gLat"), Round(Ph.GetLateralG(), 1000)).Num(TEXT("gLon"), Round(Ph.GetLongitudinalG(), 1000))
			.Num(TEXT("slipFront"), Round(Ph.GetFrontSlipAngle(), 1e4)).Num(TEXT("slipRear"), Round(Ph.GetRearSlipAngle(), 1e4))
			.Num(TEXT("wheelsOnTrack"), OnTrack).Num(TEXT("draft"), Round(Ph.GetDraftFactor(), 1000))
			.Num(TEXT("steerAngle"), Round(-Ph.GetSteerAngle(), 1e4))
			.Num(TEXT("lap"), Entry.Lap()).Num(TEXT("lapsCompleted"), Entry.LapsCompleted())
			.Num(TEXT("lapTime"), Round(Entry.CurrentLapTime(Laps.GetTime()), 1000))
			.Num(TEXT("lastLap"), Entry.LastLapTime).Num(TEXT("bestLap"), Entry.BestLapTime)
			.Num(TEXT("position"), Entry.Position).Bool(TEXT("finished"), Entry.bFinished)
			.Num(TEXT("redline"), Spec.RedlineRpm)
			.EndObj();

		O.Key(TEXT("ahead")).Arr();
		for (float X : Ahead)
		{
			const float Sa = FTrackSpline::WrapS(S + X);
			O.Arr().Num(X).Num(Round(Track.Curvature(Sa), 1e6)).Num(Round(Track.MinD(Sa), 100)).Num(Round(Track.MaxD(Sa), 100)).EndArr();
		}
		O.EndArr();

		O.Key(TEXT("rays")).Arr();
		for (int32 I = 0; I < Rays; I++)
		{
			const float A = FMath::DegreesToRadians(-RaySpreadDeg + 2.f * RaySpreadDeg * I / (Rays - 1));
			O.Num(Round(Ray(Track, S, D, Herr + A), 100));
		}
		O.EndArr();

		TArray<int32> Near;
		for (int32 I = 0; I < Field.Num(); I++)
		{
			if (Field[I] != &Car && FMath::Abs(FTrackSpline::DeltaS(S, Field[I]->S)) < OpponentRange) Near.Add(I);
		}
		Near.Sort([&](int32 A, int32 B) { return FMath::Abs(FTrackSpline::DeltaS(S, Field[A]->S)) < FMath::Abs(FTrackSpline::DeltaS(S, Field[B]->S)); });
		O.Key(TEXT("opponents")).Arr();
		for (int32 K = 0; K < FMath::Min(Opponents, Near.Num()); K++)
		{
			const FCarCore& C = *Field[Near[K]];
			const float Ds = FTrackSpline::DeltaS(S, C.S);
			const float Rel = C.ForwardSpeed() - Ph.GetForwardSpeed();
			const float Closing = Ds > 0 ? -Rel : Rel;
			const float Ttc = Closing > 0.1f ? FMath::Abs(Ds) / Closing : -1.f;
			O.Obj().Num(TEXT("id"), Near[K] + 1).Num(TEXT("ds"), Round(Ds, 100)).Num(TEXT("d"), Round(C.D, 100))
				.Num(TEXT("dd"), Round(C.D - D, 100)).Num(TEXT("relSpeed"), Round(Rel, 100))
				.Num(TEXT("ttc"), Round(Ttc, 100)).Num(TEXT("headingErr"), Round(HeadingError(C, Track), 1e4)).EndObj();
		}
		O.EndArr();

		static const TCHAR* PhaseNames[] = { TEXT("grid"), TEXT("lights"), TEXT("green"), TEXT("finished") };
		O.Key(TEXT("race")).Obj().Str(TEXT("phase"), PhaseNames[(int32)Phase]).Num(TEXT("lightsOn"), LightsOn)
			.Num(TEXT("lapsTotal"), Laps.GetRaceLaps() == MAX_int32 ? -1 : Laps.GetRaceLaps())
			.Num(TEXT("cars"), Field.Num()).Num(TEXT("raceTime"), Round(FMath::Max(0.f, Laps.RaceTime()), 1000)).EndObj();
		O.EndObj();
		return O.ToString();
	}
}

// ---------------------------------------------------------------- connection

FBotConnection::FBotConnection(FSocket* InSocket, int32 InId) : Socket(InSocket), ConnId(InId)
{
	Socket->SetNoDelay(true);
	Socket->SetNonBlocking(true);
}

FBotConnection::~FBotConnection()
{
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
	}
}

bool FBotConnection::Latest(FControl& Out) const
{
	FScopeLock L(&Lock);
	if (Control.Tick < 0) return false;
	Out = Control;
	return true;
}

void FBotConnection::Send(const FString& Line)
{
	if (bOpen) Outbox.Enqueue(Line + TEXT("\n"));
}

bool FBotConnection::Pump()
{
	if (!bOpen) return false;
	// send everything queued
	FString Line;
	while (Outbox.Dequeue(Line))
	{
		FTCHARToUTF8 Utf8(*Line);
		const uint8* Data = (const uint8*)Utf8.Get();
		int32 Left = Utf8.Length();
		while (Left > 0)
		{
			int32 Sent = 0;
			if (!Socket->Send(Data, Left, Sent))
			{
				if (ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode() == SE_EWOULDBLOCK) { FPlatformProcess::Sleep(0.0005f); continue; }
				bOpen = false;
				return false;
			}
			Data += Sent;
			Left -= Sent;
		}
	}
	// read what has arrived
	uint8 Buf[16384];
	while (true)
	{
		int32 Read = 0;
		if (!Socket->Recv(Buf, sizeof(Buf), Read, ESocketReceiveFlags::None))
		{
			const ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
			if (Err == SE_EWOULDBLOCK || Err == SE_NO_ERROR) break;
			bOpen = false;
			return false;
		}
		if (Read == 0)
		{
			// a non-blocking read of nothing: closed if the connection says so
			if (Socket->GetConnectionState() != SCS_Connected) { bOpen = false; return false; }
			break;
		}
		for (int32 I = 0; I < Read; I++)
		{
			if (Buf[I] != '\n')
			{
				if (Inbox.Num() >= BotProtocol::MaxLine)
				{
					UE_LOG(LogTemp, Warning, TEXT("Bot %s: line longer than %d bytes, disconnecting"), *Name(), BotProtocol::MaxLine);
					bOpen = false;
					return false;
				}
				Inbox.Add(Buf[I]);
				continue;
			}
			Inbox.Add(0);
			const FString Text = FString(UTF8_TO_TCHAR((const ANSICHAR*)Inbox.GetData())).TrimStartAndEnd();
			Inbox.Reset();
			if (!Text.IsEmpty()) Handle(Text);
		}
	}
	return true;
}

void FBotConnection::Handle(const FString& Line)
{
	TSharedPtr<FJsonObject> M;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, M) || !M.IsValid())
	{
		if (ProtocolErrors++ < 5) UE_LOG(LogTemp, Warning, TEXT("Bot %s: bad message"), *Name());
		return;
	}
	const FString Type = M->GetStringField(TEXT("type"));
	auto Num = [&](const TCHAR* K, double Def)
	{
		double V;
		return M->TryGetNumberField(K, V) && FMath::IsFinite(V) ? V : Def;
	};
	if (Type == TEXT("hello"))
	{
		FString N;
		M->TryGetStringField(TEXT("name"), N);
		FString Clean;
		for (TCHAR C : N) if (FChar::IsAlnum(C) || C == ' ' || C == '_' || C == '.' || C == '-') Clean.AppendChar(C);
		Clean = Clean.TrimStartAndEnd().Left(12);
		FScopeLock L(&Lock);
		BotName = Clean.IsEmpty() ? TEXT("Bot") : Clean;
		bHello = true;
	}
	else if (Type == TEXT("control"))
	{
		FScopeLock L(&Lock);
		const bool bPrev = Control.Tick >= 0;
		FControl C;
		C.Tick = (int64)Num(TEXT("tick"), -1);
		// a missing / NaN value holds the previous one; everything is clamped
		C.Steer = -FMath::Clamp((float)Num(TEXT("steer"), bPrev ? -Control.Steer : 0.f), -1.f, 1.f);
		C.Throttle = FMath::Clamp((float)Num(TEXT("throttle"), bPrev ? Control.Throttle : 0.f), 0.f, 1.f);
		C.Brake = FMath::Clamp((float)Num(TEXT("brake"), bPrev ? Control.Brake : 0.f), 0.f, 1.f);
		C.Shift = (int32)FMath::Sign(Num(TEXT("shift"), 0));
		const TSharedPtr<FJsonObject>* Dbg;
		const TArray<TSharedPtr<FJsonValue>>* LinesJson;
		if (M->TryGetObjectField(TEXT("debug"), Dbg) && (*Dbg)->TryGetArrayField(TEXT("lines"), LinesJson))
		{
			for (const TSharedPtr<FJsonValue>& V : *LinesJson)
			{
				const TArray<TSharedPtr<FJsonValue>>* Row;
				if (C.Lines.Num() >= 64 || !V->TryGetArray(Row) || Row->Num() < 4) continue;
				double P[4];
				bool bOk = true;
				for (int32 K = 0; K < 4; K++) bOk &= (*Row)[K]->TryGetNumber(P[K]) && FMath::IsFinite(P[K]);
				if (!bOk) continue;
				FColor Col = FColor::Green;
				FString Hex;
				if (Row->Num() > 4 && (*Row)[4]->TryGetString(Hex) && Hex.Len() == 7 && Hex[0] == '#') Col = FColor::FromHex(Hex);
				C.Lines.Add(FVector4f(P[0], P[1], P[2], P[3]));
				C.LineColors.Add(Col);
			}
		}
		Control = MoveTemp(C);
	}
	else if (ProtocolErrors++ < 5)
	{
		UE_LOG(LogTemp, Warning, TEXT("Bot %s: unknown message type %s"), *Name(), *Type);
	}
}

// ---------------------------------------------------------------- server

int32 FBotServer::ConfiguredPort()
{
	int32 Port = -1;
	if (FParse::Value(FCommandLine::Get(), TEXT("Bots="), Port)) return Port;
	if (FParse::Param(FCommandLine::Get(), TEXT("Bots"))) return BotProtocol::DefaultPort;
	return -1;
}

TUniquePtr<FBotServer> FBotServer::Start(int32 Port)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* L = SS->CreateSocket(NAME_Stream, TEXT("RacingSimBots"), false);
	if (!L) return nullptr;
	TSharedRef<FInternetAddr> Addr = SS->CreateInternetAddr();
	Addr->SetLoopbackAddress();
	Addr->SetPort(Port);
	L->SetReuseAddr(true);
	if (!L->Bind(*Addr) || !L->Listen(16))
	{
		UE_LOG(LogTemp, Error, TEXT("Bot server: cannot listen on 127.0.0.1:%d"), Port);
		SS->DestroySocket(L);
		return nullptr;
	}
	L->SetNonBlocking(true);
	TUniquePtr<FBotServer> S(new FBotServer());
	S->Listener = L;
	S->Port = Port;
	S->Thread = FRunnableThread::Create(S.Get(), TEXT("BotServer"));
	UE_LOG(LogTemp, Display, TEXT("Bot server listening on 127.0.0.1:%d"), Port);
	return S;
}

FBotServer::~FBotServer()
{
	bStop = true;
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
	}
	{
		FScopeLock L(&Lock);
		Bots.Reset();
	}
	if (Listener)
	{
		Listener->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Listener);
	}
}

TArray<TSharedPtr<FBotConnection>> FBotServer::Ready() const
{
	FScopeLock L(&Lock);
	TArray<TSharedPtr<FBotConnection>> R;
	for (const TSharedPtr<FBotConnection>& B : Bots) if (B->IsOpen() && B->HelloReceived()) R.Add(B);
	return R;
}

uint32 FBotServer::Run()
{
	while (!bStop)
	{
		bool bPending = false;
		if (Listener->HasPendingConnection(bPending) && bPending)
		{
			if (FSocket* C = Listener->Accept(TEXT("RacingSimBot")))
			{
				FScopeLock L(&Lock);
				Bots.Add(MakeShared<FBotConnection>(C, NextId++));
			}
		}
		TArray<TSharedPtr<FBotConnection>> Snapshot;
		{
			FScopeLock L(&Lock);
			Bots.RemoveAll([](const TSharedPtr<FBotConnection>& B) { return !B->IsOpen() && B.IsUnique(); });
			Snapshot = Bots;
		}
		for (const TSharedPtr<FBotConnection>& B : Snapshot) B->Pump();
		FPlatformProcess::Sleep(0.0005f);
	}
	return 0;
}

// ---------------------------------------------------------------- external driver

FExternalDriver::FExternalDriver(TSharedPtr<FBotConnection> InConn, FCarCore* InCar, int32 InControlHz)
	: Conn(InConn), CarCore(InCar)
{
	ControlHz = FMath::Clamp(InControlHz, 10, BotProtocol::PhysicsHz);
	StepsPerControl = FMath::Max(1, FMath::RoundToInt(BotProtocol::PhysicsHz / (float)ControlHz));
}

void FExternalDriver::DebugLines(TArray<FVector4f>& Lines, TArray<FColor>& Colors) const
{
	FScopeLock L(&DebugLock);
	Lines = DebugLineData;
	Colors = DebugColors;
}

void FExternalDriver::Welcome(int32 CarId, const FString& Mode, const FTrackSpline& Track)
{
	Tick = 0;
	Steps = 0;
	bLost = false;
	bHasHeld = false;
	const FVehicleSpec& Spec = CarCore->Spec();
	BotProtocol::FOut O;
	O.Obj().Str(TEXT("type"), TEXT("welcome")).Num(TEXT("protocol"), BotProtocol::Protocol).Num(TEXT("carId"), CarId)
		.Str(TEXT("name"), Conn->Name()).Str(TEXT("mode"), Mode).Num(TEXT("controlHz"), ControlHz).Num(TEXT("physicsHz"), BotProtocol::PhysicsHz)
		.Key(TEXT("car")).Obj().Str(TEXT("type"), Spec.Type == ECarType::Formula ? TEXT("formula") : TEXT("indy"))
		.Num(TEXT("mass"), Spec.Mass).Num(TEXT("redline"), Spec.RedlineRpm).Num(TEXT("gears"), Spec.GearRatios.Num())
		.Num(TEXT("wing"), CarCore->Sim->GetAero().DownforceLevel).Num(TEXT("maxBrakeDecel"), Spec.MaxBrakeDecel)
		.Num(TEXT("tireMu"), Spec.TireMu).EndObj().EndObj();
	Conn->Send(O.ToString());
	Conn->Send(BotProtocol::TrackMessage(Track));
}

void FExternalDriver::Update(float Dt, const FTrackSpline& Track, const TArray<FCarCore*>& Field, const FLapTracker& Laps,
	const FLapTracker::FEntry& Entry, BotProtocol::EPhase Phase, int32 LightsOn, bool bDrive)
{
	if (bLost) return;
	if (Steps++ % StepsPerControl == 0)
	{
		Tick++;
		Conn->Send(BotProtocol::StateMessage(Tick, Laps.GetTime(), *CarCore, Track, Field, Laps, Entry, Phase, LightsOn));
	}
	FBotConnection::FControl C;
	if (Conn->Latest(C))
	{
		Held = MoveTemp(C);
		bHasHeld = true;
		FScopeLock L(&DebugLock);
		DebugLineData = Held.Lines;
		DebugColors = Held.LineColors;
	}
	if (!Conn->IsOpen()) { Lose(TEXT("disconnected")); return; }
	const int64 Answered = bHasHeld ? Held.Tick : 0;
	if (Tick - Answered > 2LL * ControlHz) { Lose(TEXT("no answer for 2 s")); return; }
	if (!bDrive) return;
	FVehicleInput& In = CarCore->Input();
	if (!bHasHeld)
	{
		In.Clear();
		In.Brake = 0.3f;
		return;
	}
	In.Steer = Held.Steer;
	In.Throttle = Held.Throttle;
	In.Brake = Held.Brake;
	if (Held.Shift != 0 && Held.Tick != AppliedShiftTick)
	{
		AppliedShiftTick = Held.Tick;
		if (Held.Shift > 0) In.bShiftUpRequest = true;
		else In.bShiftDownRequest = true;
	}
}

void FExternalDriver::Lose(const TCHAR* Why)
{
	bLost = true;
	UE_LOG(LogTemp, Display, TEXT("Bot %s lost (%s): its car goes back to %s"), *Conn->Name(), Why,
		CarCore->bPlayer ? TEXT("the player") : TEXT("the built-in AI"));
	CarCore->Input().Clear();
	CarCore->Input().Brake = 0.3f;
	FScopeLock L(&DebugLock);
	DebugLineData.Reset();
	DebugColors.Reset();
}

void FExternalDriver::Event(const TCHAR* Kind, const FString& Fields)
{
	FString Line = FString::Printf(TEXT("{\"type\":\"event\",\"event\":\"%s\",\"tick\":%lld"), Kind, (long long)Tick);
	if (!Fields.IsEmpty()) Line += TEXT(",") + Fields;
	Conn->Send(Line + TEXT("}"));
}
