// Copyright 2026 Team Beaver. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transit/Handler/WPTransitHandler.h"

class ACharacter;
class UCapsuleComponent;
class UCharacterMovementComponent;
class USpringArmComponent;
class UWPTransitComponent;

struct FWPTransitRun;
struct FWPTransform;

/**
 * @brief Handles Transit for a Character whose CMC moves the Root Capsule.
 *
 * A Controller, Skeletal Mesh, Camera, and Spring Arm are optional.
 * Control Rotation is mapped when a Controller exists, and owning-client correction is sent only through a compatible PlayerController.
 * Camera Lag state is reset after Transit only when a compatible active Spring Arm is available.
 * Ragdoll and partial Physics Asset collision are not supported.
 */
class FWPCharacterHandler : public IWPTransitHandler
{
public:
	/** @brief Returns the current World-space velocity stored by the CMC. */
	virtual FVector GetVelocity(const UWPTransitComponent* TransitComp) const override;

	/** @brief Returns whether the entire Root Capsule has crossed inside the Source tangent plane. */
	virtual bool HasPassed(const FWPTransitRun& Run, const FVector& SourceSurface,
		const FVector& EntryNormal, const FVector& DestSurface, const FVector& ExitNormal) const override;
	
	/** @brief Maps Character location and velocity, plus Control Rotation when a Controller exists. */
	virtual bool Commit(FWPTransitRun& Run, const FWPTransform& Mapping) const override;
	
	/**
	 * @brief Applies the server-authoritative Character Transit result to the owning
	 *        Client.
	 * @param TransitComp Transit Component owned by the Client Character.
	 * @param DestLocation Server-authoritative Capsule location.
	 * @param DestRotation Server-authoritative Character rotation.
	 * @param DestVelocity CMC Velocity transformed by the Portal rotation.
	 * @param DestControl Control Rotation transformed by the Portal rotation.
	 */
	static void SyncClient(UWPTransitComponent* TransitComp, const FVector& DestLocation, const FRotator& DestRotation, const FVector& DestVelocity, const FRotator& DestControl);
	
private:
	/** @brief Returns the Transit Component's Owner as a Character. */
	static ACharacter* GetCharacter(const UWPTransitComponent* TransitComp);
	
	/** @brief Returns the Character Movement selected by the Resolver. */
	static UCharacterMovementComponent* GetMove(const UWPTransitComponent* TransitComp);
	
	/** @brief Returns the Root Capsule actually moved by the CMC. */
	static UCapsuleComponent* GetRoot(const UWPTransitComponent* TransitComp, const UCharacterMovementComponent* MoveComp);
	
	/**
	 * @brief Finds the active Camera Spring Arm whose Camera Lag state can be reset safely.
	 * @param Master Character to search for a Spring Arm.
	 * @return Compatible Spring Arm, or nullptr if none is available or the Character does
	 *         not use a Camera.
	 */
	static USpringArmComponent* FindActiveSpringArm(ACharacter* Master);
	
	/** @brief Resets the Spring Arm's previous state so the pre-Transit location does not remain in Camera Lag history. */
	static void ResetArm(USpringArmComponent* SpringArm);
};

