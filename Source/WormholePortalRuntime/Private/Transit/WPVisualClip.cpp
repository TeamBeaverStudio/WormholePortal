// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Transit/WPVisualClip.h"

#include "Components/PrimitiveComponent.h"
#include "Math/Float16.h"

#include "WPSettings.h"

void FWPVisualClip::SetSource(UPrimitiveComponent* Part, const FVector& SourceCenter, const FVector& EntryNormal,
	float Radius)
{
	SetData(Part, SourceCenter, EntryNormal, Radius);
}

void FWPVisualClip::SetTwin(UPrimitiveComponent* Part, const FVector& DestCenter, const FVector& ExitNormal,
	float Radius)
{
	SetData(Part, DestCenter, ExitNormal, Radius);
}

uint32 FWPVisualClip::PackData(const FVector& Normal, float Radius)
{
	if (!FMath::IsFinite(Radius) || Radius <= KINDA_SMALL_NUMBER) return 0;
	
	const FVector SafeNormal = Normal.GetSafeNormal();
	if (SafeNormal.IsNearlyZero()) return 0;
	
	const FVector2f OctNormal = EncodeNormal(SafeNormal);
	const float NormalX01 = FMath::Clamp(OctNormal.X * 0.5f + 0.5f, 0.0f, 1.0f);
	const float NormalY01 = FMath::Clamp(OctNormal.Y * 0.5f + 0.5f, 0.0f, 1.0f);
	
	const uint32 NormalX = static_cast<uint32>(FMath::Clamp(FMath::RoundToInt(NormalX01 * 255.0f), 0, 255));
	const uint32 NormalY = static_cast<uint32>(FMath::Clamp(FMath::RoundToInt(NormalY01 * 255.0f), 0, 255));
	
	FFloat16 RadiusHalf;
	RadiusHalf.SetClamped(Radius);
	
	// [RadiusHalf][NormalY][NormalX]
	return NormalX | (NormalY << 8u) | (static_cast<uint32>(RadiusHalf.Encoded) << 16u);
}

void FWPVisualClip::SetPacked(UPrimitiveComponent* Part, const FVector3f& Center, uint32 PackedData)
{
	if (!IsValid(Part)) return;
	
	if (PackedData == 0)
	{
		Clear(Part);
		return;
	}
	
	const FVector4f ClipData(Center.X, Center.Y, Center.Z, FPlatformMath::AsFloat(PackedData));
	Part->SetCustomPrimitiveDataVector4f(GetDataIndex(), ClipData);
}

void FWPVisualClip::Clear(UPrimitiveComponent* Part)
{
	if (IsValid(Part))
	{
		Part->SetCustomPrimitiveDataVector4f(GetDataIndex(), FVector4f::Zero());
	}
}

FVector2f FWPVisualClip::EncodeNormal(const FVector& Normal)
{
	const float NormalX = static_cast<float>(Normal.X);
	const float NormalY = static_cast<float>(Normal.Y);
	const float NormalZ = static_cast<float>(Normal.Z);
	
	const float LengthL1 = FMath::Abs(NormalX) + FMath::Abs(NormalY) + FMath::Abs(NormalZ);
	FVector2f EncodedNormal(NormalX/LengthL1, NormalY/LengthL1);
	
	if (NormalZ < 0.0f)
	{
		const float PreviousX = EncodedNormal.X;
		const float PreviousY = EncodedNormal.Y;
		
		const float SignX = PreviousX >= 0.0f ? 1.0f : -1.0f;
		const float SignY = PreviousY >= 0.0f ? 1.0f : -1.0f;
		
		EncodedNormal.X = (1.0f - FMath::Abs(PreviousY)) * SignX;
		EncodedNormal.Y = (1.0f - FMath::Abs(PreviousX)) * SignY;
	}
	
	return EncodedNormal;
}

void FWPVisualClip::SetData(UPrimitiveComponent* Part, const FVector& Center, const FVector& Normal, float Radius)
{
	if (!IsValid(Part)) return;
	
	const uint32 PackedData = PackData(Normal, Radius);
	
	SetPacked(Part, FVector3f(Center), PackedData);
}

int32 FWPVisualClip::GetDataIndex()
{
	return GetDefault<UWPSettings>()->ClipBaseIndex;
}
