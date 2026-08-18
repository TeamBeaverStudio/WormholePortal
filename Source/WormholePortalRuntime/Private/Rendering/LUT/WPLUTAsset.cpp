// Copyright 2026 Team Beaver. All Rights Reserved.

#include "Rendering/LUT/WPLUTAsset.h"

#include "Engine/VolumeTexture.h"

namespace
{
	bool Fail(FString* OutError, const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}
}

bool UWPLUTAsset::Validate(FString* OutError) const
{
	FString DescriptorError;
	if (!Descriptor.IsValid(&DescriptorError))
	{
		return Fail(OutError, FString::Printf(TEXT("Invalid descriptor: %s"), *DescriptorError));
	}

	if (BuildHash != Descriptor.MakeBuildHash())
	{
		return Fail(OutError, TEXT("BuildHash does not match Descriptor."));
	}

	if (!VolumeTexture)
	{
		return Fail(OutError, TEXT("VolumeTexture is null."));
	}

	const FIntVector Dimensions = Descriptor.GetDimensions();
	if (VolumeTexture->GetSizeX() != Dimensions.X
		|| VolumeTexture->GetSizeY() != Dimensions.Y
		|| VolumeTexture->GetSizeZ() != Dimensions.Z)
	{
		return Fail(OutError, FString::Printf(
			TEXT("Volume dimensions do not match Descriptor (%dx%dx%d)."),
			Dimensions.X, Dimensions.Y, Dimensions.Z));
	}

	if (VolumeTexture->GetPixelFormat() != PF_A32B32G32R32F)
	{
		return Fail(OutError, TEXT("VolumeTexture must use PF_A32B32G32R32F."));
	}

	const int32 NumMips = VolumeTexture->GetNumMips();
	if (NumMips != 1)
	{
		return Fail(OutError, FString::Printf(
			TEXT("VolumeTexture must contain exactly one mip (actual: %d)."),
			NumMips));
	}

	if (VolumeTexture->SRGB)
	{
		return Fail(OutError, TEXT("VolumeTexture must have sRGB disabled."));
	}

	if (OutError)
	{
		OutError->Reset();
	}

	return true;
}

/** check if the LUT asset is compatible with the desired descriptor */
bool UWPLUTAsset::IsCompatibleWith(
	const FWPLUTDescriptor& DesiredDescriptor,
	FString* OutError) const
{
	if (!Validate(OutError))
	{
		return false;
	}

	if (Descriptor != DesiredDescriptor)
	{
		return Fail(OutError, TEXT("Descriptor does not exactly match the requested build."));
	}

	return true;
}


