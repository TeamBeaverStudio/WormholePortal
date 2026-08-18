// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/LUT/WPLUTTypes.h"

#include "Misc/Crc.h"
#include "Misc/SecureHash.h"

namespace
{
	constexpr int32 MinImpactSamples = 16;
	constexpr int32 MaxImpactSamples = 2048;
	constexpr int32 MinTransitionSamples = 2;
	constexpr int32 MaxTransitionSamples = 512;
	constexpr int32 MinRatioSamples = 2;
	constexpr int32 MaxRatioSamples = 128;
	constexpr int32 MinIntegrationSteps = 16;
	constexpr int32 MaxIntegrationSteps = 2048;
	constexpr float MinimumRatio = 0.01f;
	constexpr float MaximumRatio = 1024.0f;

	void SetError(FString* OutError, const TCHAR* Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
	}

	FString BuildCanonicalDescriptorString(const FWPLUTDescriptor& Descriptor)
	{
		const FWPLUTDescriptor D = Descriptor.GetSanitized();
		return FString::Printf(
			TEXT("WPLUT|Size=%d,%d,%d|Steps=%d|Ratio=%.9g,%.9g|Flatten=%.9g"),
			D.ImpactSamples,
			D.TransitionSamples,
			D.RatioSamples,
			D.IntegrationSteps,
			static_cast<double>(D.TransitionRatioMin),
			static_cast<double>(D.TransitionRatioMax),
			static_cast<double>(D.TailFlattenStartFraction));
	}
}

FWPLUTDescriptor FWPLUTDescriptor::MakeDefault()
{
	return FWPLUTDescriptor();
}

FWPLUTDescriptor FWPLUTDescriptor::GetSanitized() const
{
	FWPLUTDescriptor Result = *this;
	Result.ImpactSamples = FMath::Clamp(ImpactSamples, MinImpactSamples, MaxImpactSamples);

	if ((Result.ImpactSamples & 1) != 0)
	{
		Result.ImpactSamples = FMath::Min(Result.ImpactSamples + 1, MaxImpactSamples);
	}

	Result.TransitionSamples = FMath::Clamp(TransitionSamples, MinTransitionSamples, MaxTransitionSamples);
	Result.RatioSamples = FMath::Clamp(RatioSamples, MinRatioSamples, MaxRatioSamples);
	Result.IntegrationSteps = FMath::Clamp(IntegrationSteps, MinIntegrationSteps, MaxIntegrationSteps);
	Result.TransitionRatioMin = FMath::Clamp(TransitionRatioMin, MinimumRatio, MaximumRatio);
	Result.TransitionRatioMax = FMath::Clamp(TransitionRatioMax, MinimumRatio, MaximumRatio);

	if (Result.TransitionRatioMax <= Result.TransitionRatioMin)
	{
		Result.TransitionRatioMax = FMath::Min(
			MaximumRatio,
			Result.TransitionRatioMin * 1.01f + KINDA_SMALL_NUMBER);

		if (Result.TransitionRatioMax <= Result.TransitionRatioMin)
		{
			Result.TransitionRatioMin = Result.TransitionRatioMax / 1.01f;
		}
	}

	Result.TailFlattenStartFraction = FMath::Clamp(TailFlattenStartFraction, 0.1f, 0.95f);

	return Result;
}

bool FWPLUTDescriptor::IsValid(FString* OutError) const
{
	if (ImpactSamples < MinImpactSamples || ImpactSamples > MaxImpactSamples || (ImpactSamples & 1) != 0)
	{
		SetError(OutError, TEXT("ImpactSamples must be even and in [16, 2048]."));
		return false;
	}

	if (TransitionSamples < MinTransitionSamples || TransitionSamples > MaxTransitionSamples
		|| RatioSamples < MinRatioSamples || RatioSamples > MaxRatioSamples)
	{
		SetError(OutError, TEXT("TransitionSamples or RatioSamples is outside the supported range."));
		return false;
	}

	if (IntegrationSteps < MinIntegrationSteps || IntegrationSteps > MaxIntegrationSteps)
	{
		SetError(OutError, TEXT("IntegrationSteps is outside [16, 2048]."));
		return false;
	}

	if (!FMath::IsFinite(TransitionRatioMin) || !FMath::IsFinite(TransitionRatioMax)
		|| TransitionRatioMin < MinimumRatio || TransitionRatioMax <= TransitionRatioMin
		|| TransitionRatioMax > MaximumRatio)
	{
		SetError(OutError, TEXT("Transition ratio domain is invalid."));
		return false;
	}

	if (!FMath::IsFinite(TailFlattenStartFraction)
		|| TailFlattenStartFraction < 0.1f || TailFlattenStartFraction > 0.95f)
	{
		SetError(OutError, TEXT("TailFlattenStartFraction is outside [0.1, 0.95]."));
		return false;
	}

	if (OutError)
	{
		OutError->Reset();
	}

	return true;
}

bool FWPLUTDescriptor::ContainsTransitionRatio(const float TransitionRatio, const float RelativeTolerance) const
{
	if (!FMath::IsFinite(TransitionRatio) || TransitionRatio <= 0.0f)
	{
		return false;
	}

	const FWPLUTDescriptor D = GetSanitized();
	const float Tolerance = FMath::Max(0.0f, RelativeTolerance);

	return TransitionRatio >= D.TransitionRatioMin * (1.0f - Tolerance)
		&& TransitionRatio <= D.TransitionRatioMax * (1.0f + Tolerance);
}

float FWPLUTDescriptor::TransitionRatioFromCoordinate01(const float Coordinate01) const
{
	const FWPLUTDescriptor D = GetSanitized();

	return D.TransitionRatioMin * FMath::Pow(
		D.TransitionRatioMax / D.TransitionRatioMin,
		FMath::Clamp(Coordinate01, 0.0f, 1.0f));
}

float FWPLUTDescriptor::ComputeRatioCoordinate01(const float TransitionRatio) const
{
	const FWPLUTDescriptor D = GetSanitized();
	const float SafeRatio = FMath::Clamp(TransitionRatio, D.TransitionRatioMin, D.TransitionRatioMax);

	return FMath::Loge(SafeRatio / D.TransitionRatioMin)
		/ FMath::Loge(D.TransitionRatioMax / D.TransitionRatioMin);
}

float FWPLUTDescriptor::RatioFromSlice(const int32 SliceIndex) const
{
	const FWPLUTDescriptor D = GetSanitized();
	const int32 SafeSlice = FMath::Clamp(SliceIndex, 0, D.RatioSamples - 1);
	const float Coordinate = static_cast<float>(SafeSlice) / static_cast<float>(D.RatioSamples - 1);

	return D.TransitionRatioFromCoordinate01(Coordinate);
}

float FWPLUTDescriptor::ClampTransitionRatio(float TransitionRatio) const
{
	const FWPLUTDescriptor D = GetSanitized();
	
	if (!FMath::IsFinite(TransitionRatio))
	{
		return D.TransitionRatioMin;
	}
	
	return FMath::Clamp(
		TransitionRatio,
		D.TransitionRatioMin,
		D.TransitionRatioMax);
}

FIntVector FWPLUTDescriptor::GetDimensions() const
{
	const FWPLUTDescriptor D = GetSanitized();

	return FIntVector(D.ImpactSamples, D.TransitionSamples, D.RatioSamples);
}

bool FWPLUTDescriptor::IsSamplingCompatible(const FWPLUTDescriptor& Other) const
{
	const FWPLUTDescriptor A = GetSanitized();
	const FWPLUTDescriptor B = Other.GetSanitized();

	return A.ImpactSamples == B.ImpactSamples
		&& A.TransitionSamples == B.TransitionSamples
		&& A.RatioSamples == B.RatioSamples
		&& A.IntegrationSteps == B.IntegrationSteps
		&& A.TailFlattenStartFraction == B.TailFlattenStartFraction;
}

FString FWPLUTDescriptor::MakeBuildHash() const
{
	const FString Canonical = BuildCanonicalDescriptorString(*this);
	FTCHARToUTF8 Utf8(*Canonical);
	const FSHAHash Hash = FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()));

	return Hash.ToString();
}

bool FWPLUTDescriptor::operator==(const FWPLUTDescriptor& Other) const
{
	const FWPLUTDescriptor A = GetSanitized();
	const FWPLUTDescriptor B = Other.GetSanitized();

	return A.IsSamplingCompatible(B)
		&& A.TransitionRatioMin == B.TransitionRatioMin
		&& A.TransitionRatioMax == B.TransitionRatioMax;
}

uint32 GetTypeHash(const FWPLUTDescriptor& Descriptor)
{
	return FCrc::StrCrc32(*Descriptor.MakeBuildHash());
}

bool FWPLUTVolumeData::IsValid(FString* OutError) const
{
	FString DescriptorError;
	if (!Descriptor.IsValid(&DescriptorError))
	{
		SetError(OutError, *DescriptorError);
		return false;
	}

	if (Dimensions != Descriptor.GetDimensions())
	{
		SetError(OutError, TEXT("Volume dimensions do not match the descriptor."));
		return false;
	}

	const int64 ExpectedVoxels = static_cast<int64>(Dimensions.X) * Dimensions.Y * Dimensions.Z;
	if (ExpectedVoxels <= 0 || ExpectedVoxels != Voxels.Num())
	{
		SetError(OutError, TEXT("Volume voxel count is invalid."));
		return false;
	}

	if (BuildHash != Descriptor.MakeBuildHash())
	{
		SetError(OutError, TEXT("Volume build hash does not match the descriptor."));
		return false;
	}

	if (OutError)
	{
		OutError->Reset();
	}

	return true;
}
