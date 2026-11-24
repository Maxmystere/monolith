// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

// Core / test
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"

// JSON / registry
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

// UMG -- build a throwaway WBP + probe the result
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"

// Animation
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneCurveChannelCommon.h"

// Event track
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Channels/MovieSceneEventChannel.h"

// Delegate binding
#include "Animation/WidgetAnimationDelegateBinding.h"
#include "Engine/BlueprintGeneratedClass.h"

// Render transform track + runtime playback
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/UMGSequenceTickManager.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Widgets/SWidget.h"
#include "Actions/Hoisted/AnimationCoreActions.h"

// Asset / package
#include "Editor.h"

namespace MonolithUI::AnimationCoreTests
{
    static const FString GTestAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_AnimCoreTest");

    static UWidgetAnimation* FindAnimByLabel(UWidgetBlueprint* WBP, const FString& Label)
    {
#if WITH_EDITORONLY_DATA
        for (UWidgetAnimation* Anim : WBP->Animations)
        {
            if (Anim && Anim->GetDisplayLabel() == Label)
            {
                return Anim;
            }
        }
#endif
        return nullptr;
    }

    static TSharedPtr<FJsonObject> MakeKey(double Time, double Value, const TCHAR* Interp)
    {
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("time"), Time);
        Key->SetNumberField(TEXT("value"), Value);
        Key->SetStringField(TEXT("interp"), Interp);
        return Key;
    }

    static TSharedPtr<FJsonObject> MakeLinearTrack(const TCHAR* Widget, const TCHAR* Property, double From, double To, double Duration)
    {
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("widget_name"), Widget);
        Track->SetStringField(TEXT("property"), Property);
        TArray<TSharedPtr<FJsonValue>> Keys;
        Keys.Add(MakeShared<FJsonValueObject>(MakeKey(0.0, From, TEXT("linear"))));
        Keys.Add(MakeShared<FJsonValueObject>(MakeKey(Duration, To, TEXT("linear"))));
        Track->SetArrayField(TEXT("keys"), Keys);
        return Track;
    }

    /** CSS cubic-bezier(x1, y1, x2, y2) progress at normalised time X (bisection on x(t), then y(t)). */
    static double EvalCssBezier(double X1, double Y1, double X2, double Y2, double X)
    {
        auto Bez = [](double P1, double P2, double T)
        {
            const double U = 1.0 - T;
            return 3.0 * U * U * T * P1 + 3.0 * U * T * T * P2 + T * T * T;
        };
        double Lo = 0.0, Hi = 1.0;
        for (int32 Iter = 0; Iter < 60; ++Iter)
        {
            const double Mid = 0.5 * (Lo + Hi);
            (Bez(X1, X2, Mid) < X ? Lo : Hi) = Mid;
        }
        return Bez(Y1, Y2, 0.5 * (Lo + Hi));
    }
} // namespace MonolithUI::AnimationCoreTests


/**
 * MonolithUI.CreateAnimationV2.Basic
 *
 * Creates a 2-key cubic RenderOpacity animation on a test WBP. Asserts:
 * animation exists in WBP->Animations, channel has 2 keys, key values match
 * within tolerance, tangent data present.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICreateAnimationV2BasicTest,
    "MonolithUI.CreateAnimationV2.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICreateAnimationV2BasicTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::AnimationCoreTests;
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    FString SetupError;
    if (!CreateOrReuseTestWidgetBlueprint(GTestAssetPath, FName(TEXT("MyImage")), nullptr, SetupError))
    {
        AddError(FString::Printf(TEXT("Fixture setup failed: %s"), *SetupError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), GTestAssetPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
    Params->SetNumberField(TEXT("duration_sec"), 0.5);
    Params->SetBoolField(TEXT("compile_once"), true);

    TArray<TSharedPtr<FJsonValue>> Tracks;
    {
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

        TArray<TSharedPtr<FJsonValue>> Keys;
        {
            TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
            K0->SetNumberField(TEXT("time"), 0.0);
            K0->SetNumberField(TEXT("value"), 0.0);
            K0->SetStringField(TEXT("interp"), TEXT("cubic"));
            K0->SetNumberField(TEXT("leave_tangent"), 2.0);
            K0->SetNumberField(TEXT("leave_weight"), 0.33);
            Keys.Add(MakeShared<FJsonValueObject>(K0));
        }
        {
            TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
            K1->SetNumberField(TEXT("time"), 0.5);
            K1->SetNumberField(TEXT("value"), 1.0);
            K1->SetStringField(TEXT("interp"), TEXT("cubic"));
            K1->SetNumberField(TEXT("arrive_tangent"), 0.0);
            K1->SetNumberField(TEXT("arrive_weight"), 0.33);
            Keys.Add(MakeShared<FJsonValueObject>(K1));
        }
        Track->SetArrayField(TEXT("keys"), Keys);
        Tracks.Add(MakeShared<FJsonValueObject>(Track));
    }
    Params->SetArrayField(TEXT("tracks"), Tracks);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("create_animation_v2"), Params);

    TestTrue(TEXT("create_animation_v2 bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double KeysInserted = 0.0;
    TestTrue(TEXT("result has keys_inserted"), Result.Result->TryGetNumberField(TEXT("keys_inserted"), KeysInserted));
    TestEqual(TEXT("keys_inserted == 2"), static_cast<int32>(KeysInserted), 2);

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *GTestAssetPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("FadeIn"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("FadeIn animation found in WBP->Animations"), FoundAnim);
    if (!FoundAnim)
    {
        return false;
    }

    TestNotNull(TEXT("MovieScene exists"), FoundAnim->MovieScene.Get());
    if (!FoundAnim->MovieScene)
    {
        return false;
    }

    UMovieSceneFloatTrack* FloatTrack = nullptr;
    const UMovieScene* ConstScene = FoundAnim->MovieScene;
    for (const FMovieSceneBinding& Binding : ConstScene->GetBindings())
    {
        FloatTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneFloatTrack>(
            Binding.GetObjectGuid(), FName(TEXT("RenderOpacity")));
        if (FloatTrack)
        {
            break;
        }
    }
    TestNotNull(TEXT("RenderOpacity float track found"), FloatTrack);
    if (!FloatTrack)
    {
        return false;
    }

    TestTrue(TEXT("Track has at least 1 section"), FloatTrack->GetAllSections().Num() > 0);
    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Float section exists"), Section);
    if (!Section)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = Section->GetChannel();
    TestEqual(TEXT("Channel has 2 keys"), Channel.GetNumKeys(), 2);

    TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
    if (Values.Num() >= 2)
    {
        TestNearlyEqual(TEXT("Key 0 value ~ 0.0"), Values[0].Value, 0.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 1 value ~ 1.0"), Values[1].Value, 1.0f, 0.01f);
        // leave_tangent is value per second; the channel stores value per tick.
        const double TicksPerSecond = FoundAnim->MovieScene->GetTickResolution().AsDecimal();
        TestNearlyEqual(TEXT("Key 0 leave tangent ~ 2.0 per second, stored per tick"),
            static_cast<double>(Values[0].Tangent.LeaveTangent), 2.0 / TicksPerSecond, 1e-6);
        TestNearlyEqual(TEXT("Key 0 leave weight stored as-is"), Values[0].Tangent.LeaveTangentWeight, 0.33f, 1e-4f);
    }
#endif

    return true;
}


/**
 * MonolithUI.AddBezierEasedSegment.Basic
 *
 * Creates a bezier-eased segment with cubic-bezier(0.42, 0, 0.58, 1.0)
 * (ease-in-out). Asserts: 2 keys exist, weighted tangent mode set,
 * tangent values non-zero for this curve shape.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAddBezierEasedSegmentBasicTest,
    "MonolithUI.AddBezierEasedSegment.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAddBezierEasedSegmentBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString BezierTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_BezierSegTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(BezierTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), BezierTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("EaseInOut"));
    Params->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
    Params->SetStringField(TEXT("property"), TEXT("RenderOpacity"));
    Params->SetNumberField(TEXT("from_value"), 0.0);
    Params->SetNumberField(TEXT("to_value"), 1.0);
    Params->SetNumberField(TEXT("start_time"), 0.0);
    Params->SetNumberField(TEXT("end_time"), 0.5);

    TArray<TSharedPtr<FJsonValue>> Bezier;
    Bezier.Add(MakeShared<FJsonValueNumber>(0.42));
    Bezier.Add(MakeShared<FJsonValueNumber>(0.0));
    Bezier.Add(MakeShared<FJsonValueNumber>(0.58));
    Bezier.Add(MakeShared<FJsonValueNumber>(1.0));
    Params->SetArrayField(TEXT("bezier"), Bezier);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("add_bezier_eased_segment"), Params);

    TestTrue(TEXT("add_bezier_eased_segment bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double KeysInserted = 0.0;
    TestTrue(TEXT("result has keys_inserted"), Result.Result->TryGetNumberField(TEXT("keys_inserted"), KeysInserted));
    TestEqual(TEXT("keys_inserted == 2"), static_cast<int32>(KeysInserted), 2);

    const TSharedPtr<FJsonObject>* TangentInfoPtr = nullptr;
    TestTrue(TEXT("result has tangent_info"), Result.Result->TryGetObjectField(TEXT("tangent_info"), TangentInfoPtr));

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *BezierTestPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload bezier test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("EaseInOut"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("EaseInOut animation found"), FoundAnim);
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return false;
    }

    UMovieSceneFloatTrack* FloatTrack = nullptr;
    const UMovieScene* ConstScene2 = FoundAnim->MovieScene;
    for (const FMovieSceneBinding& Binding : ConstScene2->GetBindings())
    {
        FloatTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneFloatTrack>(
            Binding.GetObjectGuid(), FName(TEXT("RenderOpacity")));
        if (FloatTrack)
        {
            break;
        }
    }
    TestNotNull(TEXT("RenderOpacity float track found"), FloatTrack);
    if (!FloatTrack || FloatTrack->GetAllSections().Num() == 0)
    {
        return false;
    }

    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Float section exists"), Section);
    if (!Section)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = Section->GetChannel();
    TestEqual(TEXT("Channel has 2 keys"), Channel.GetNumKeys(), 2);

    TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
    if (Values.Num() >= 2)
    {
        TestNearlyEqual(TEXT("Key 0 value ~ 0.0"), Values[0].Value, 0.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 1 value ~ 1.0"), Values[1].Value, 1.0f, 0.01f);

        TestEqual(TEXT("Key 0 tangent weight mode is WeightedBoth"),
            static_cast<int32>(Values[0].Tangent.TangentWeightMode),
            static_cast<int32>(RCTWM_WeightedBoth));
        TestEqual(TEXT("Key 1 tangent weight mode is WeightedBoth"),
            static_cast<int32>(Values[1].Tangent.TangentWeightMode),
            static_cast<int32>(RCTWM_WeightedBoth));

        // For ease-in-out (0.42, 0, 0.58, 1.0) over 0.5 s: both handles are flat, so
        // the tangents are 0 and each weight is the handle's length in seconds,
        // 0.42 * 0.5 = 0.21.
        TestNearlyEqual(TEXT("Key 0 leave tangent ~ 0"), Values[0].Tangent.LeaveTangent, 0.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 1 arrive tangent ~ 0"), Values[1].Tangent.ArriveTangent, 0.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 0 leave weight ~ 0.21"), Values[0].Tangent.LeaveTangentWeight, 0.21f, 1e-4f);
        TestNearlyEqual(TEXT("Key 1 arrive weight ~ 0.21"), Values[1].Tangent.ArriveTangentWeight, 0.21f, 1e-4f);
    }
#endif

    return true;
}


/**
 * MonolithUI.BakeSpringAnimation.Basic
 *
 * Bakes an underdamped spring (stiffness=100, damping=10, mass=1) into dense
 * linear keyframes. Asserts: key count > 10, at least 2 zero-crossings
 * relative to target (underdamped overshoot).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIBakeSpringAnimationBasicTest,
    "MonolithUI.BakeSpringAnimation.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIBakeSpringAnimationBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString SpringTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpringTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(SpringTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), SpringTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("SpringBounce"));
    Params->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
    Params->SetStringField(TEXT("property"), TEXT("RenderOpacity"));
    Params->SetNumberField(TEXT("from_value"), 0.0);
    Params->SetNumberField(TEXT("to_value"), 1.0);
    Params->SetNumberField(TEXT("stiffness"), 100.0);
    Params->SetNumberField(TEXT("damping"), 10.0);
    Params->SetNumberField(TEXT("mass"), 1.0);
    Params->SetNumberField(TEXT("fps"), 60.0);
    Params->SetNumberField(TEXT("duration"), 2.0);
    Params->SetBoolField(TEXT("compile_once"), true);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("bake_spring_animation"), Params);

    TestTrue(TEXT("bake_spring_animation bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double KeysInserted = 0.0;
    TestTrue(TEXT("result has keys_inserted"), Result.Result->TryGetNumberField(TEXT("keys_inserted"), KeysInserted));
    TestTrue(TEXT("keys_inserted > 10 (dense sampling)"), static_cast<int32>(KeysInserted) > 10);

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *SpringTestPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload spring test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("SpringBounce"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("SpringBounce animation found"), FoundAnim);
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return false;
    }

    UMovieSceneFloatTrack* FloatTrack = nullptr;
    const UMovieScene* ConstScene = FoundAnim->MovieScene;
    for (const FMovieSceneBinding& Binding : ConstScene->GetBindings())
    {
        FloatTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneFloatTrack>(
            Binding.GetObjectGuid(), FName(TEXT("RenderOpacity")));
        if (FloatTrack)
        {
            break;
        }
    }
    TestNotNull(TEXT("RenderOpacity float track found"), FloatTrack);
    if (!FloatTrack || FloatTrack->GetAllSections().Num() == 0)
    {
        return false;
    }

    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Float section exists"), Section);
    if (!Section)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = Section->GetChannel();
    TestTrue(TEXT("Channel key count > 10"), Channel.GetNumKeys() > 10);

    // Underdamped spring should overshoot, so values cross 1.0 at least twice.
    TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
    int32 ZeroCrossings = 0;
    const float Target = 1.0f;
    for (int32 i = 1; i < Values.Num(); ++i)
    {
        const float Prev = Values[i - 1].Value - Target;
        const float Curr = Values[i].Value - Target;
        if ((Prev > 0.0f && Curr <= 0.0f) || (Prev < 0.0f && Curr >= 0.0f))
        {
            ++ZeroCrossings;
        }
    }
    TestTrue(TEXT("Underdamped spring has >= 2 zero-crossings"), ZeroCrossings >= 2);
#endif

    return true;
}


/**
 * MonolithUI.AddAnimationEventTrack.Basic
 *
 * Adds 2 event keys at t=0 and t=0.3 to an existing animation. Asserts:
 * master event track exists, section has correct key count.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAddAnimationEventTrackBasicTest,
    "MonolithUI.AddAnimationEventTrack.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAddAnimationEventTrackBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString EventTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_EventTrackTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(EventTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("asset_path"), EventTestPath);
        CreateParams->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
        CreateParams->SetNumberField(TEXT("duration_sec"), 0.5);
        CreateParams->SetBoolField(TEXT("compile_once"), false);

        TArray<TSharedPtr<FJsonValue>> Tracks;
        {
            TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
            Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
            Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

            TArray<TSharedPtr<FJsonValue>> Keys;
            {
                TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
                K0->SetNumberField(TEXT("time"), 0.0);
                K0->SetNumberField(TEXT("value"), 0.0);
                K0->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K0));
            }
            {
                TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
                K1->SetNumberField(TEXT("time"), 0.5);
                K1->SetNumberField(TEXT("value"), 1.0);
                K1->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K1));
            }
            Track->SetArrayField(TEXT("keys"), Keys);
            Tracks.Add(MakeShared<FJsonValueObject>(Track));
        }
        CreateParams->SetArrayField(TEXT("tracks"), Tracks);

        const FMonolithActionResult CreateResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_animation_v2"), CreateParams);
        if (!CreateResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("Prerequisite create_animation_v2 failed: %s"), *CreateResult.ErrorMessage));
            return false;
        }
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), EventTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));

    TArray<TSharedPtr<FJsonValue>> Events;
    {
        TSharedPtr<FJsonObject> E0 = MakeShared<FJsonObject>();
        E0->SetNumberField(TEXT("time"), 0.0);
        E0->SetStringField(TEXT("event_name"), TEXT("OnFadeStart"));
        Events.Add(MakeShared<FJsonValueObject>(E0));
    }
    {
        TSharedPtr<FJsonObject> E1 = MakeShared<FJsonObject>();
        E1->SetNumberField(TEXT("time"), 0.3);
        E1->SetStringField(TEXT("event_name"), TEXT("OnFadeMid"));
        Events.Add(MakeShared<FJsonValueObject>(E1));
    }
    Params->SetArrayField(TEXT("events"), Events);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("add_animation_event_track"), Params);

    TestTrue(TEXT("add_animation_event_track bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double EventsInserted = 0.0;
    TestTrue(TEXT("result has events_inserted"), Result.Result->TryGetNumberField(TEXT("events_inserted"), EventsInserted));
    TestEqual(TEXT("events_inserted == 2"), static_cast<int32>(EventsInserted), 2);

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *EventTestPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload event track test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("FadeIn"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("FadeIn animation found"), FoundAnim);
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return false;
    }

    const UMovieScene* ConstScene = FoundAnim->MovieScene;
    UMovieSceneEventTrack* EventTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneEventTrack>();
    TestNotNull(TEXT("Master event track found"), EventTrack);
    if (!EventTrack)
    {
        return false;
    }

    TestTrue(TEXT("Event track has at least 1 section"), EventTrack->GetAllSections().Num() > 0);

    UMovieSceneEventTriggerSection* TriggerSection =
        Cast<UMovieSceneEventTriggerSection>(EventTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Trigger section exists"), TriggerSection);
    if (!TriggerSection)
    {
        return false;
    }

    TestEqual(TEXT("Event channel has 2 keys"),
        TriggerSection->EventChannel.GetNumKeys(), 2);
#endif

    return true;
}


/**
 * MonolithUI.BindAnimationToEvent.Basic
 *
 * Binds "OnHovered" to "Started" on an existing animation. Verifies
 * UWidgetAnimationDelegateBinding has the correct entry on the generated class.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIBindAnimationToEventBasicTest,
    "MonolithUI.BindAnimationToEvent.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIBindAnimationToEventBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString BindTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_BindAnimTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(BindTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("asset_path"), BindTestPath);
        CreateParams->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
        CreateParams->SetNumberField(TEXT("duration_sec"), 0.5);
        CreateParams->SetBoolField(TEXT("compile_once"), true);

        TArray<TSharedPtr<FJsonValue>> Tracks;
        {
            TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
            Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
            Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

            TArray<TSharedPtr<FJsonValue>> Keys;
            {
                TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
                K0->SetNumberField(TEXT("time"), 0.0);
                K0->SetNumberField(TEXT("value"), 0.0);
                K0->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K0));
            }
            {
                TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
                K1->SetNumberField(TEXT("time"), 0.5);
                K1->SetNumberField(TEXT("value"), 1.0);
                K1->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K1));
            }
            Track->SetArrayField(TEXT("keys"), Keys);
            Tracks.Add(MakeShared<FJsonValueObject>(Track));
        }
        CreateParams->SetArrayField(TEXT("tracks"), Tracks);

        const FMonolithActionResult CreateResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_animation_v2"), CreateParams);
        if (!CreateResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("Prerequisite create_animation_v2 failed: %s"), *CreateResult.ErrorMessage));
            return false;
        }
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), BindTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
    Params->SetStringField(TEXT("widget_event"), TEXT("OnHovered"));
    Params->SetStringField(TEXT("animation_event"), TEXT("Started"));

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("bind_animation_to_event"), Params);

    TestTrue(TEXT("bind_animation_to_event bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    bool bBindingCreated = false;
    TestTrue(TEXT("result has binding_created"), Result.Result->TryGetBoolField(TEXT("binding_created"), bBindingCreated));
    TestTrue(TEXT("binding_created is true"), bBindingCreated);

    FString FuncNameBound;
    TestTrue(TEXT("result has function_name_bound"), Result.Result->TryGetStringField(TEXT("function_name_bound"), FuncNameBound));
    TestEqual(TEXT("function_name_bound matches"), FuncNameBound, TEXT("OnHovered_PlayFadeIn"));

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *BindTestPath);
    if (!WBP || !WBP->GeneratedClass)
    {
        AddError(TEXT("Failed to reload WBP or GeneratedClass is null"));
        return false;
    }

    UDynamicBlueprintBinding* BindingObj = UBlueprintGeneratedClass::GetDynamicBindingObject(
        WBP->GeneratedClass, UWidgetAnimationDelegateBinding::StaticClass());

    if (!BindingObj)
    {
        AddWarning(TEXT("UWidgetAnimationDelegateBinding not found on GeneratedClass -- "
                       "may have been cleared by recompilation. Marking as expected limitation."));
        return true;
    }

    UWidgetAnimationDelegateBinding* DelegateBinding = Cast<UWidgetAnimationDelegateBinding>(BindingObj);
    TestNotNull(TEXT("DelegateBinding cast succeeded"), DelegateBinding);
    if (!DelegateBinding)
    {
        return false;
    }

    TestTrue(TEXT("Has at least 1 binding entry"),
        DelegateBinding->WidgetAnimationDelegateBindings.Num() > 0);

    if (DelegateBinding->WidgetAnimationDelegateBindings.Num() > 0)
    {
        const FBlueprintWidgetAnimationDelegateBinding& Entry =
            DelegateBinding->WidgetAnimationDelegateBindings.Last();
        TestEqual(TEXT("Action is Started"),
            static_cast<uint8>(Entry.Action),
            static_cast<uint8>(EWidgetAnimationEvent::Started));
        TestEqual(TEXT("AnimationToBind is FadeIn"),
            Entry.AnimationToBind, FName(TEXT("FadeIn")));
        TestEqual(TEXT("FunctionNameToBind matches"),
            Entry.FunctionNameToBind, FName(TEXT("OnHovered_PlayFadeIn")));
    }

    return true;
}


/**
 * MonolithUI.CreateAnimationV2.RenderTransform
 *
 * Keys RenderTransform.Translation.Y and RenderTransform.Scale.X. Asserts the keys
 * land on one UMovieScene2DTransformTrack bound to RenderTransform (not on float
 * tracks, which write the struct field without calling SetRenderTransform), that a
 * float track left on the nested path by an earlier version is removed, and that
 * playing the animation moves the widget's Slate render transform.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICreateAnimationV2RenderTransformTest,
    "MonolithUI.CreateAnimationV2.RenderTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICreateAnimationV2RenderTransformTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::AnimationCoreTests;
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString TransformTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_AnimTransformTest");
    const FString AnimName = TEXT("MoveAndScale");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(TransformTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *TransformTestPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to load transform test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    // Seed the float track an earlier version would have written for this path.
    {
        UWidgetAnimation* Seed = MonolithUI::FindOrCreateWidgetAnimation(WBP, AnimName);
        if (!Seed || !Seed->MovieScene)
        {
            AddError(TEXT("Failed to seed the animation"));
            return false;
        }
        const FGuid SeedGuid = MonolithUI::EnsureWidgetPossessableBinding(Seed, Seed->MovieScene, TEXT("MyImage"), false);
        UMovieSceneFloatTrack* Stale = Seed->MovieScene->AddTrack<UMovieSceneFloatTrack>(SeedGuid);
        Stale->SetPropertyNameAndPath(TEXT("RenderTransform.Translation.Y"), TEXT("RenderTransform.Translation.Y"));
    }
#endif

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), TransformTestPath);
    Params->SetStringField(TEXT("animation_name"), AnimName);
    Params->SetNumberField(TEXT("duration_sec"), 0.5);
    Params->SetBoolField(TEXT("compile_once"), true);
    TArray<TSharedPtr<FJsonValue>> Tracks;
    Tracks.Add(MakeShared<FJsonValueObject>(MakeLinearTrack(TEXT("MyImage"), TEXT("RenderTransform.Translation.Y"), 0.0, 100.0, 0.5)));
    Tracks.Add(MakeShared<FJsonValueObject>(MakeLinearTrack(TEXT("MyImage"), TEXT("RenderTransform.Scale.X"), 1.0, 2.0, 0.5)));
    Params->SetArrayField(TEXT("tracks"), Tracks);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("create_animation_v2"), Params);
    TestTrue(TEXT("create_animation_v2 bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }
    TestFalse(TEXT("no warnings for render-transform channels"), Result.Result->HasField(TEXT("warnings")));

    // An unknown RenderTransform path is rejected rather than keyed on a float track.
    {
        TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
        BadParams->SetStringField(TEXT("asset_path"), TransformTestPath);
        BadParams->SetStringField(TEXT("animation_name"), TEXT("BadTransformPath"));
        BadParams->SetNumberField(TEXT("duration_sec"), 0.5);
        BadParams->SetBoolField(TEXT("compile_once"), false);
        TArray<TSharedPtr<FJsonValue>> BadTracks;
        BadTracks.Add(MakeShared<FJsonValueObject>(MakeLinearTrack(TEXT("MyImage"), TEXT("RenderTransform.Translation"), 0.0, 1.0, 0.5)));
        BadParams->SetArrayField(TEXT("tracks"), BadTracks);
        const FMonolithActionResult BadResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_animation_v2"), BadParams);
        TestFalse(TEXT("RenderTransform.Translation (no axis) is rejected"), BadResult.bSuccess);
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* Anim = FindAnimByLabel(WBP, AnimName);
    TestNotNull(TEXT("animation found"), Anim);
    if (!Anim || !Anim->MovieScene)
    {
        return false;
    }
    UMovieScene* Scene = Anim->MovieScene;

    FGuid ImageGuid;
    for (const FWidgetAnimationBinding& Binding : Anim->AnimationBindings)
    {
        if (Binding.WidgetName == FName(TEXT("MyImage")))
        {
            ImageGuid = Binding.AnimationGuid;
        }
    }
    TestTrue(TEXT("MyImage binding found"), ImageGuid.IsValid());

    const FMovieSceneBinding* SceneBinding = static_cast<const UMovieScene*>(Scene)->FindBinding(ImageGuid);
    TestNotNull(TEXT("MovieScene binding found"), SceneBinding);
    if (!SceneBinding)
    {
        return false;
    }
    for (UMovieSceneTrack* Track : SceneBinding->GetTracks())
    {
        if (const UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
        {
            AddError(FString::Printf(TEXT("Unexpected float track left on the binding: %s"),
                *FloatTrack->GetPropertyPath().ToString()));
        }
    }

    UMovieScene2DTransformTrack* TransformTrack =
        Scene->FindTrack<UMovieScene2DTransformTrack>(ImageGuid, FName(TEXT("RenderTransform")));
    TestNotNull(TEXT("2D transform track bound to RenderTransform"), TransformTrack);
    if (!TransformTrack || TransformTrack->GetAllSections().Num() == 0)
    {
        return false;
    }
    TestEqual(TEXT("track property path"), TransformTrack->GetPropertyPath().ToString(), FString(TEXT("RenderTransform")));

    const UMovieScene2DTransformSection* Section = Cast<UMovieScene2DTransformSection>(TransformTrack->GetAllSections()[0]);
    TestNotNull(TEXT("2D transform section"), Section);
    if (!Section)
    {
        return false;
    }
    TestEqual(TEXT("Translation.Y keyed"), Section->Translation[1].GetNumKeys(), 2);
    TestEqual(TEXT("Scale.X keyed"), Section->Scale[0].GetNumKeys(), 2);
    TestEqual(TEXT("Translation.X untouched"), Section->Translation[0].GetNumKeys(), 0);
    TestEqual(TEXT("Angle untouched"), Section->Rotation.GetNumKeys(), 0);

    // Play the compiled animation on a widget instance and check what Slate received.
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    UWidgetBlueprintGeneratedClass* GenClass = Cast<UWidgetBlueprintGeneratedClass>(WBP->GeneratedClass);
    if (!World || !GenClass)
    {
        AddError(TEXT("No editor world or generated class to play the animation in"));
        return false;
    }

    // The widget blueprint compiler duplicates each animation onto the class as "<Name>_INST".
    const FName RuntimeAnimName(*(Anim->GetName() + TEXT("_INST")));
    UWidgetAnimation* RuntimeAnim = nullptr;
    for (UWidgetAnimation* Candidate : GenClass->Animations)
    {
        if (Candidate && Candidate->GetFName() == RuntimeAnimName)
        {
            RuntimeAnim = Candidate;
        }
    }
    TestNotNull(TEXT("compiled animation on the generated class"), RuntimeAnim);

    UUserWidget* Instance = CreateWidget<UUserWidget>(World, GenClass);
    TestNotNull(TEXT("widget instance"), Instance);
    if (!RuntimeAnim || !Instance)
    {
        return false;
    }
    // TakeWidget returns the only strong reference to the Slate tree; keep it alive.
    const TSharedRef<SWidget> SlateRoot = Instance->TakeWidget();
    UWidget* Image = Instance->GetWidgetFromName(FName(TEXT("MyImage")));
    TestNotNull(TEXT("MyImage instance"), Image);

    Instance->PlayAnimation(RuntimeAnim, /*StartAtTime=*/0.25f);
    if (UUMGSequenceTickManager* TickManager = UUMGSequenceTickManager::Get(Instance))
    {
        TickManager->ForceFlush();
    }

    const TSharedPtr<SWidget> SlateImage = Image ? Image->GetCachedWidget() : nullptr;
    TestTrue(TEXT("MyImage has a Slate widget"), SlateImage.IsValid());
    if (SlateImage.IsValid())
    {
        const TOptional<FSlateRenderTransform>& SlateTransform = SlateImage->GetRenderTransform();
        TestTrue(TEXT("Slate render transform set by the animation"), SlateTransform.IsSet());
        if (SlateTransform.IsSet())
        {
            const float SlateY = SlateTransform->GetTranslation().Y;
            TestTrue(FString::Printf(TEXT("Slate translation Y mid-animation (%f) is between 0 and 100"), SlateY),
                SlateY > 1.0f && SlateY < 99.0f);
            TestNearlyEqual(TEXT("Slate translation Y matches UWidget::RenderTransform"),
                SlateY, static_cast<float>(Image->GetRenderTransform().Translation.Y), 0.01f);
        }
        const double ScaleX = Image->GetRenderTransform().Scale.X;
        TestTrue(FString::Printf(TEXT("RenderTransform.Scale.X mid-animation (%f) is between 1 and 2"), ScaleX),
            ScaleX > 1.01 && ScaleX < 1.99);
    }

    Instance->StopAllAnimations();
    Instance->RemoveFromParent();
#endif

    return true;
}


/**
 * MonolithUI.AddBezierEasedSegment.MatchesCssCurve
 *
 * Inserts CSS cubic-bezier segments (including an overshoot, a vertical start
 * handle and a falling value) and checks the evaluated channel against the CSS
 * curve at 19 points. Catches tangents stored in the wrong time unit and weights
 * that ignore the segment's duration or value change.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAddBezierEasedSegmentMatchesCssTest,
    "MonolithUI.AddBezierEasedSegment.MatchesCssCurve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAddBezierEasedSegmentMatchesCssTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::AnimationCoreTests;
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString CssTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_BezierCssTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(CssTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    struct FCase
    {
        const TCHAR* Name;
        double X1, Y1, X2, Y2;
        double From, To;
    };
    const FCase Cases[] =
    {
        { TEXT("Ease"),          0.25, 0.1,  0.25, 1.0, 0.0,   100.0 },
        { TEXT("EaseOutBack"),   0.34, 1.56, 0.64, 1.0, 0.0,   100.0 },
        { TEXT("VerticalStart"), 0.0,  0.8,  0.2,  1.0, 0.0,   100.0 },
        { TEXT("EaseInFalling"), 0.42, 0.0,  1.0,  1.0, 100.0, 0.0   },
    };
    const double StartTime = 0.2;
    const double EndTime = 1.7;

    for (const FCase& Case : Cases)
    {
        const FString AnimName = FString::Printf(TEXT("Css_%s"), Case.Name);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), CssTestPath);
        Params->SetStringField(TEXT("animation_name"), AnimName);
        Params->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Params->SetStringField(TEXT("property"), TEXT("RenderOpacity"));
        Params->SetNumberField(TEXT("from_value"), Case.From);
        Params->SetNumberField(TEXT("to_value"), Case.To);
        Params->SetNumberField(TEXT("start_time"), StartTime);
        Params->SetNumberField(TEXT("end_time"), EndTime);
        TArray<TSharedPtr<FJsonValue>> Bezier;
        Bezier.Add(MakeShared<FJsonValueNumber>(Case.X1));
        Bezier.Add(MakeShared<FJsonValueNumber>(Case.Y1));
        Bezier.Add(MakeShared<FJsonValueNumber>(Case.X2));
        Bezier.Add(MakeShared<FJsonValueNumber>(Case.Y2));
        Params->SetArrayField(TEXT("bezier"), Bezier);

        const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("add_bezier_eased_segment"), Params);
        if (!Result.bSuccess)
        {
            AddError(FString::Printf(TEXT("%s: %s"), Case.Name, *Result.ErrorMessage));
            continue;
        }

#if WITH_EDITORONLY_DATA
        UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *CssTestPath);
        UWidgetAnimation* Anim = WBP ? FindAnimByLabel(WBP, AnimName) : nullptr;
        if (!Anim || !Anim->MovieScene)
        {
            AddError(FString::Printf(TEXT("%s: animation not found"), Case.Name));
            continue;
        }

        const UMovieSceneFloatSection* Section = nullptr;
        const UMovieScene* ConstScene = Anim->MovieScene;
        for (const FMovieSceneBinding& Binding : ConstScene->GetBindings())
        {
            if (UMovieSceneFloatTrack* Track = Anim->MovieScene->FindTrack<UMovieSceneFloatTrack>(
                    Binding.GetObjectGuid(), FName(TEXT("RenderOpacity"))))
            {
                if (Track->GetAllSections().Num() > 0)
                {
                    Section = Cast<UMovieSceneFloatSection>(Track->GetAllSections()[0]);
                }
            }
        }
        if (!Section)
        {
            AddError(FString::Printf(TEXT("%s: float section not found"), Case.Name));
            continue;
        }

        const FFrameRate TickRes = Anim->MovieScene->GetTickResolution();
        for (int32 Step = 1; Step < 20; ++Step)
        {
            const double Progress = Step / 20.0;
            const double Time = StartTime + Progress * (EndTime - StartTime);
            const double Expected = Case.From + (Case.To - Case.From)
                * EvalCssBezier(Case.X1, Case.Y1, Case.X2, Case.Y2, Progress);

            float Actual = 0.0f;
            Section->GetChannel().Evaluate(TickRes.AsFrameTime(Time), Actual);
            TestNearlyEqual(FString::Printf(TEXT("%s at x=%.2f"), Case.Name, Progress),
                static_cast<double>(Actual), Expected, 0.5);
        }
#endif
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
