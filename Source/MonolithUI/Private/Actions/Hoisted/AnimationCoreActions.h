// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
// Phase I: TRange<FFrameNumber> + FFrameRate + FGuid appear in helper signatures
// below as return-by-value or const-ref-template-instantiation. Forward-decls
// are insufficient for those cases — include the small POD headers directly.
#include "Misc/FrameRate.h"
#include "Math/Range.h"
#include "Misc/Guid.h"

class UMovieScene;
class UWidgetAnimation;
class UWidgetBlueprint;
class UWidget;
struct FMovieSceneFloatChannel;

/**
 * MonolithUI -- UMG widget animation authoring (multi-track, weighted-tangent, spring-bake).
 *
 * create_animation_v2:        Creates a UWidgetAnimation on a WBP with multi-track,
 *                             multi-key cubic / linear / constant keyframe data.
 *                             Cubic supports tangent weight for CSS-style bezier easing.
 * add_bezier_eased_segment:   Convenience wrapper that converts a cubic-bezier(x1,y1,x2,y2)
 *                             control-point pair into UE weighted tangents and inserts the
 *                             resulting 2-key segment into an existing or new animation.
 * bake_spring_animation:      Bakes a damped harmonic spring into dense linear keyframes
 *                             (semi-implicit Euler with convergence early-out).
 *
 * Editor-only -- UWidgetBlueprint::Animations is WITH_EDITORONLY_DATA.
 *
 * Phase I exposure: the helper namespace `MonolithUI::AnimationInternal` (defined in the
 * .cpp) is mirrored here as a thin public-to-MonolithUI surface so the new
 * `FUIAnimationMovieSceneBuilder` (Public/Animation/) can call it without duplicating
 * the CSS-bezier-to-weighted-tangent math, the possessable-binding plumbing, or the
 * track/section/channel ensure routine. The action handlers continue to use the
 * same helpers via the .cpp-internal namespace; the public mirror simply forwards.
 */
namespace MonolithUI
{
    struct FAnimationCoreActions
    {
        static void Register(FMonolithToolRegistry& Registry);

        static FMonolithActionResult HandleCreateAnimationV2(const TSharedPtr<FJsonObject>& Params);
        static FMonolithActionResult HandleAddBezierEasedSegment(const TSharedPtr<FJsonObject>& Params);
        static FMonolithActionResult HandleBakeSpringAnimation(const TSharedPtr<FJsonObject>& Params);
    };

    /**
     * One UMG-keyframe descriptor — neutral to the original-action JSON shape so
     * builders can populate it from an `FUISpecKeyframe` directly.
     *
     * Mirrors `MonolithUI::AnimationInternal::FParsedKey` (.cpp) field-for-field.
     * Kept in the public header so the MovieScene builder can build one without
     * including the .cpp's anonymous helpers.
     */
    struct MONOLITHUI_API FAnimationCoreKey
    {
        /** Seconds. */
        double Time = 0.0;
        float Value = 0.0f;
        /** "cubic" / "linear" / "constant". */
        FString Interp = TEXT("cubic");
        /**
         * Slopes in value units per SECOND. `InsertFloatChannelKey` divides them by
         * the tick resolution, because `FMovieSceneTangentData` stores value per tick.
         */
        float ArriveTangent = 0.0f;
        float LeaveTangent = 0.0f;
        /**
         * Handle lengths, measured in (seconds, value) space: the engine's weighted
         * cubic puts the handle tip at key + (cos a, sin a) * Weight, where
         * tan a = the per-second slope. Stored as-is.
         */
        float ArriveTangentWeight = 0.0f;
        float LeaveTangentWeight = 0.0f;
        bool bHasWeights = false;
    };

    /**
     * CSS cubic-bezier-to-weighted-tangent conversion.
     *
     * Given a CSS-style `cubic-bezier(x1, y1, x2, y2)` control-point pair, plus the
     * segment's from/to values and start/end times, fill OutKey0 (segment start) and
     * OutKey1 (segment end) so the engine's weighted cubic reproduces the CSS curve.
     *
     * The CSS control points are the Bezier handles in normalised (time, progress)
     * space. They are scaled into (seconds, value) space: the leave handle of key 0
     * is (x1 * Duration, y1 * DeltaValue), the arrive handle of key 1 is
     * ((1 - x2) * Duration, (1 - y2) * DeltaValue). Each key gets that handle's slope
     * (value per second, see FAnimationCoreKey) and its length as the weight. A
     * vertical handle (x1 == 0 or x2 == 1) is kept a hair off vertical so the slope
     * stays finite.
     *
     * Both keys are returned with `Interp = "cubic"` and `bHasWeights = true`.
     */
    MONOLITHUI_API void ComputeBezierWeightedTangents(
        double X1, double Y1, double X2, double Y2,
        double FromValue, double ToValue,
        double StartTime, double EndTime,
        FAnimationCoreKey& OutKey0,
        FAnimationCoreKey& OutKey1);

    /**
     * Find an existing UWidgetAnimation by display label, or create+register a new one
     * (including the deterministic-GUID entry the WBP compiler validates against).
     * Returns nullptr on a non-editor build.
     */
    MONOLITHUI_API UWidgetAnimation* FindOrCreateWidgetAnimation(
        UWidgetBlueprint* WBP,
        const FString& AnimationName);

    /**
     * Ensure a possessable binding exists for the given widget on the supplied
     * UWidgetAnimation/MovieScene. Returns the binding GUID. Idempotent.
     */
    MONOLITHUI_API FGuid EnsureWidgetPossessableBinding(
        UWidgetAnimation* WidgetAnim,
        UMovieScene* Scene,
        const FString& WidgetName,
        bool bIsRootWidget);

    /**
     * Index of the UMovieScene2DTransformSection channel a render-transform path
     * names, in the section's channel order (Translation.X, Translation.Y, Angle,
     * Scale.X, Scale.Y, Shear.X, Shear.Y), or INDEX_NONE. Paths are
     * `RenderTransform.<Field>[.X|.Y]`, matched case-insensitively.
     */
    MONOLITHUI_API int32 GetRenderTransformChannelIndex(const FString& PropertyPath);

    /**
     * Find or create the track + first section that animates `PropertyName` on the
     * possessable identified by `PossessableGuid`, and return the float channel to
     * key. Re-uses the existing section if present; when bResetExistingKeys is true
     * the returned channel's keys are cleared, for idempotent re-builds.
     *
     * Render-transform paths (see GetRenderTransformChannelIndex) are keyed on the
     * matching channel of a UMovieScene2DTransformTrack bound to `RenderTransform`.
     * That track goes through UWidget::SetRenderTransform, which pushes the value to
     * Slate; a float track on the nested path would only write the struct field, so
     * the widget would not move on screen. A float track left on the same nested
     * path by an earlier version is removed. Any other `RenderTransform*` path is an
     * error.
     *
     * Every other property gets a float track. OutMessage, when given, receives the
     * error when nullptr is returned, or a warning (otherwise empty) when a nested
     * path is keyed on a float track, since the engine writes such paths without
     * calling the widget's setter.
     */
    MONOLITHUI_API FMovieSceneFloatChannel* EnsureFloatTrackSectionChannel(
        UMovieScene* Scene,
        const FGuid& PossessableGuid,
        const FString& PropertyName,
        const TRange<FFrameNumber>& SectionRange,
        bool bResetExistingKeys = true,
        FString* OutMessage = nullptr);

    /** Insert a single key into a float channel. Tangents follow FAnimationCoreKey's units. */
    MONOLITHUI_API void InsertFloatChannelKey(
        FMovieSceneFloatChannel* Channel,
        const FFrameRate& TickRes,
        const FAnimationCoreKey& Key);
}
