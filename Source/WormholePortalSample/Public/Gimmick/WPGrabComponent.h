// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PhysicsEngine/PhysicsHandleComponent.h"
#include "WPGrabComponent.generated.h"

struct FWPTransitEvent;

UCLASS(ClassGroup = (Custom), BlueprintType, meta = (BlueprintSpawnableComponent))
class WORMHOLEPORTALSAMPLE_API UWPGrabComponent : public UPhysicsHandleComponent
{
	GENERATED_BODY()

public:
	UWPGrabComponent();

	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Grab")
	bool GrabActor();

	UFUNCTION(BlueprintCallable, Category = "Wormhole Portal|Sample|Grab")
	void DropActor();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** @brief Releases the grabbed Component when its owning Actor starts Transit. */
	void HandleTransitStarted(const FWPTransitEvent& Event);

	bool GetView(FVector& OutLocation, FRotator& OutRotation) const;

	/** Handle used to remove the Transit Started subscription during EndPlay. */
	FDelegateHandle TransitStartedHandle;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Grab", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float GrabRange = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wormhole Portal|Sample|Grab", meta = (AllowPrivateAccess = "true", ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float HoldDistance = 100.0f;
};
