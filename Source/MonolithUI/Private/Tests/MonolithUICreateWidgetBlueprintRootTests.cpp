// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"

#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

#include "UMGEditorProjectSettings.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Overlay.h"
#include "Components/SafeZone.h"

namespace MonolithUI::CreateWidgetBlueprintRootTests
{
    static FMonolithActionResult Create(const FString& SavePath, const TCHAR* RootWidget)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("save_path"), SavePath);
        if (RootWidget)
        {
            Params->SetStringField(TEXT("root_widget"), RootWidget);
        }
        Params->SetBoolField(TEXT("skip_save"), true);
        return FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("create_widget_blueprint"), Params);
    }

    /** Unique per run: skip_save keeps the asset in memory, and a second create at the same path is refused. */
    static FString UniquePath(const TCHAR* Stem)
    {
        return FString::Printf(TEXT("/Game/Tests/Monolith/UI/%s_%s"), Stem, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
} // namespace MonolithUI::CreateWidgetBlueprintRootTests


/**
 * MonolithUI.CreateWidgetBlueprint.RootWidget
 *
 * With the project's Default Root Widget set to SafeZone (as some projects do), an
 * explicit root_widget must still win, an omitted one keeps the project default, the
 * result reports the root actually created, and a non-panel root_widget is refused.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICreateWidgetBlueprintRootWidgetTest,
    "MonolithUI.CreateWidgetBlueprint.RootWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICreateWidgetBlueprintRootWidgetTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::CreateWidgetBlueprintRootTests;

    UUMGEditorProjectSettings* Settings = GetMutableDefault<UUMGEditorProjectSettings>();
    const TSubclassOf<UPanelWidget> SavedDefaultRoot = Settings->DefaultRootWidget;
    const bool bSavedTemplateSelector = Settings->bUseWidgetTemplateSelector;
    ON_SCOPE_EXIT
    {
        Settings->DefaultRootWidget = SavedDefaultRoot;
        Settings->bUseWidgetTemplateSelector = bSavedTemplateSelector;
    };
    Settings->DefaultRootWidget = USafeZone::StaticClass();
    Settings->bUseWidgetTemplateSelector = false;

    // Explicit root_widget replaces the project default.
    {
        const FString Path = UniquePath(TEXT("WBP_RootOverlay"));
        const FMonolithActionResult Result = Create(Path, TEXT("Overlay"));
        TestTrue(TEXT("create with root_widget=Overlay succeeds"), Result.bSuccess);
        if (Result.bSuccess)
        {
            FString ReportedRoot;
            Result.Result->TryGetStringField(TEXT("root_widget"), ReportedRoot);
            TestEqual(TEXT("result reports Overlay"), ReportedRoot, FString(TEXT("Overlay")));

            const UWidgetBlueprint* WBP = FindObject<UWidgetBlueprint>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
            TestNotNull(TEXT("WBP exists"), WBP);
            if (WBP && WBP->WidgetTree)
            {
                TestTrue(TEXT("root is an Overlay"), Cast<UOverlay>(WBP->WidgetTree->RootWidget) != nullptr);
                TArray<UWidget*> AllWidgets;
                WBP->WidgetTree->GetAllWidgets(AllWidgets);
                TestEqual(TEXT("only the root is in the tree"), AllWidgets.Num(), 1);
                TestFalse(TEXT("the project default root is not registered as a variable"),
                    WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("SafeZone_0"))));
                TestTrue(TEXT("compiles cleanly"), WBP->Status != BS_Error);
            }
        }
    }

    // Omitted root_widget keeps the project default, and the result says so.
    {
        const FMonolithActionResult Result = Create(UniquePath(TEXT("WBP_RootDefault")), nullptr);
        TestTrue(TEXT("create without root_widget succeeds"), Result.bSuccess);
        FString ReportedRoot;
        if (Result.bSuccess)
        {
            Result.Result->TryGetStringField(TEXT("root_widget"), ReportedRoot);
        }
        TestEqual(TEXT("result reports the project default SafeZone"), ReportedRoot, FString(TEXT("SafeZone")));
    }

    // A non-panel root is refused before anything is created.
    {
        const FMonolithActionResult Result = Create(UniquePath(TEXT("WBP_RootImage")), TEXT("Image"));
        TestFalse(TEXT("root_widget=Image is refused"), Result.bSuccess);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
