// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WPPortalGunComponent.generated.h"

class AWormholePortalActor;

UENUM(BlueprintType)
enum class EWPPortalSpawnDirection : uint8
{
	PositiveX UMETA(DisplayName = "+X"),
	NegativeX UMETA(DisplayName = "-X"),
	PositiveY UMETA(DisplayName = "+Y"),
	NegativeY UMETA(DisplayName = "-Y"),
	PositiveZ UMETA(DisplayName = "+Z"),
	NegativeZ UMETA(DisplayName = "-Z"),
	Count UMETA(Hidden)
};

/** Selects whether the sample growth effect changes only rendering or the complete physical Metric. */
UENUM(BlueprintType)
enum class EWPPortalGrowthMode : uint8
{
	/** Recommended. Keeps final collision, bounds, capture resolution, LUT, and ownership state stable. */
	VisualOnly UMETA(DisplayName = "Visual Only (Recommended)"),
	/** Resizes the real Metric, collision, bounds, visibility queries, and dynamic capture resolution. */
	PhysicalMetric UMETA(DisplayName = "Physical Metric (Higher Cost)")
};

UCLASS(ClassGroup=(Custom), BlueprintType, meta=(BlueprintSpawnableComponent))
class WORMHOLEPORTALSAMPLE_API UWPPortalGunComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWPPortalGunComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Portal Gun")
	bool FirePortalA(FVector TargetPosition);
	
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Portal Gun")
	bool FirePortalB(FVector TargetPosition);
	
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Portal Gun")
	bool SetPortalA(AWormholePortalActor* NewPortal);
	
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Portal Gun")
	bool SetPortalB(AWormholePortalActor* NewPortal);

	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Sample|Portal Gun")
	bool HasPortalA() const;

	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Sample|Portal Gun")
	bool HasPortalB() const;
	
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Portal Gun|Spawn")
	void CyclePortalSpawnDirection(float WheelDelta);
	
	UFUNCTION(BlueprintPure, Category = "Wormhole Portal|Sample|Portal Gun|Spawn")
	FText GetPortalSpawnDirectionText() const;
	
	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Portal Gun")
	void ClearPortals();

private:
	bool 			FirePortal(const FVector& TargetPosition, TObjectPtr<AWormholePortalActor>& Slot, bool& bOwned);
	bool 			AssignPortal(AWormholePortalActor* NewPortal, TObjectPtr<AWormholePortalActor>& Slot, bool& bOwned, AWormholePortalActor* Other);
	void 			LinkAndStartGrowth();
	void			UpdateGrowthAnimation(float DeltaTime);
	static float	EaseGrowth(float LinearAlpha);
	static FVector	ToWorldVector(EWPPortalSpawnDirection Direction);
	/** Establishes the final physical Metric before either growth mode starts. */
	void			InitializeFinalPhysicalMetric(AWormholePortalActor* Portal) const;
	/** Routes a normalized animation scale to the explicitly selected visual or physical API. */
	void 			ApplyGrowthScale(AWormholePortalActor* Portal, float Scale) const;
	void 			ReleasePortal(TObjectPtr<AWormholePortalActor>& Portal, bool& bOwned);
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun", meta = (AllowPrivateAccess = "true"))
	TSubclassOf<AWormholePortalActor> PortalClass;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wormhole Portal|Sample|Portal Gun|Spawn", meta = (AllowPrivateAccess = "true"))
	bool bCanSpawnPortalA = true;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wormhole Portal|Sample|Portal Gun|Spawn", meta = (AllowPrivateAccess = "true"))
	bool bCanSpawnPortalB = true;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wormhole Portal|Sample|Portal Gun|Spawn", meta = (AllowPrivateAccess = "true"))
	EWPPortalSpawnDirection PortalSpawnDirection = EWPPortalSpawnDirection::PositiveX;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun|Metric", meta = (AllowPrivateAccess = "true", ClampMin = "1.0", UIMin = "1.0", Units = "cm"))
	float PortalRadius = 300.f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun|Metric", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float ThroatHalfLength = 100.0f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun|Metric", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float TransitionLength = 200.0f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun|Growth", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "s"))
	float GrowthDuration = 0.35f;
	
	/** Determines whether growth is render-only or changes the complete physical Metric. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun|Growth", meta = (AllowPrivateAccess = "true"))
	EWPPortalGrowthMode GrowthMode = EWPPortalGrowthMode::VisualOnly;

	/** Starting scale used by either growth mode before easing to 1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun|Growth", meta = (AllowPrivateAccess = "true", ClampMin = "0.01", ClampMax = "1.0", UIMin = "0.01", UIMax = "1.0"))
	float InitialGrowthScale = 0.05f;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Portal Gun|Placement", meta = (AllowPrivateAccess = "true", ClampMin = "1.0", UIMin = "1.0", Units = "cm"))
	float MaxPlacementDistance = 10000.0f;
	
	UPROPERTY(Transient)
	TObjectPtr<AWormholePortalActor> PortalA;
	
	UPROPERTY(Transient)
	TObjectPtr<AWormholePortalActor> PortalB;
	
	UPROPERTY(Transient)
	bool bOwnsPortalA = false;
	
	UPROPERTY(Transient)
	bool bOwnsPortalB = false;
	
	float GrowthElapsedSeconds = 0.0f;
};
