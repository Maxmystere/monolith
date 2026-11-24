#include "MonolithBlueprintGraphRenderer.h"

#include "MonolithBlueprintInternal.h"
#include "MonolithPinTypeGrammar.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_Literal.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_Self.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"

namespace MonolithGraphRenderDetail
{
	static bool IsExec(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	static bool IsKnot(const UEdGraphNode* Node)
	{
		return Node && Node->IsA<UK2Node_Knot>();
	}

	static bool IsComment(const UEdGraphNode* Node)
	{
		return Node && Node->IsA<UEdGraphNode_Comment>();
	}

	/** A macro graph's own entry/exit tunnel. Macro instances and collapsed-graph nodes derive from
	 *  UK2Node_Tunnel too, but they are ordinary calls in the graph that holds them. */
	static bool IsTunnelBoundary(const UEdGraphNode* Node)
	{
		return Node && Node->IsA<UK2Node_Tunnel>() && !Node->IsA<UK2Node_MacroInstance>() && !Node->IsA<UK2Node_Composite>();
	}

	static bool HasExecPins(const UEdGraphNode* Node)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (IsExec(Pin))
			{
				return true;
			}
		}
		return false;
	}

	static FString OneLine(const FString& In)
	{
		FString Out = In.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" "));
		Out.TrimStartAndEndInline();
		return Out;
	}

	static FString FirstLine(const FString& In)
	{
		FString Left, Right;
		return In.Split(TEXT("\n"), &Left, &Right) ? Left.TrimEnd() : In.TrimEnd();
	}

	static FString Quote(const FString& In)
	{
		return TEXT("\"") + In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""))
			.Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT("\\n")) + TEXT("\"");
	}

	static FString PinTypeText(const UEdGraphPin* Pin)
	{
		return MonolithPinTypeGrammar::ContainerPrefix(Pin->PinType) + MonolithPinTypeGrammar::PinTypeToString(Pin->PinType);
	}

	/** Visible data pins of a direction (exec excluded, split sub-pins excluded). */
	static TArray<UEdGraphPin*> DataPins(const UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		TArray<UEdGraphPin*> Out;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && !IsExec(Pin) && !Pin->ParentPin && (!Pin->bHidden || Pin->SubPins.Num() > 0))
			{
				Out.Add(Pin);
			}
		}
		return Out;
	}

	static TArray<UEdGraphPin*> ExecPins(const UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		TArray<UEdGraphPin*> Out;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (IsExec(Pin) && Pin->Direction == Direction && !Pin->bHidden)
			{
				Out.Add(Pin);
			}
		}
		return Out;
	}

	/** The real input pins an exec/data output reaches, through reroute knots. */
	static void ResolveTargets(const UEdGraphPin* OutPin, TArray<UEdGraphPin*>& OutTargets, int32 Guard = 0)
	{
		if (!OutPin || Guard > 64)
		{
			return;
		}
		for (UEdGraphPin* Linked : OutPin->LinkedTo)
		{
			UEdGraphNode* Owner = Linked ? Linked->GetOwningNode() : nullptr;
			if (!Owner)
			{
				continue;
			}
			if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Owner))
			{
				ResolveTargets(Knot->GetOutputPin(), OutTargets, Guard + 1);
			}
			else
			{
				OutTargets.Add(Linked);
			}
		}
	}

	/** The real output pin feeding an input pin, through reroute knots. */
	static UEdGraphPin* ResolveSource(const UEdGraphPin* InPin)
	{
		const UEdGraphPin* Current = InPin;
		for (int32 Guard = 0; Current && Guard < 64; ++Guard)
		{
			if (Current->LinkedTo.Num() == 0 || !Current->LinkedTo[0])
			{
				return nullptr;
			}
			UEdGraphPin* Source = Current->LinkedTo[0];
			UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Source->GetOwningNode());
			if (!Knot)
			{
				return Source;
			}
			Current = Knot->GetInputPin();
		}
		return nullptr;
	}

	/** Node title for statements and expressions. */
	static FString NodeName(const UEdGraphNode* Node)
	{
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
		{
			const FName Member = Call->FunctionReference.GetMemberName();
			if (!Member.IsNone())
			{
				return Member.ToString();
			}
		}
		if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
		{
			if (Macro->GetMacroGraph())
			{
				return Macro->GetMacroGraph()->GetName();
			}
		}
		if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node))
		{
			return Custom->CustomFunctionName.ToString();
		}
		if (const UK2Node_DynamicCast* Cast = ::Cast<UK2Node_DynamicCast>(Node))
		{
			return FString::Printf(TEXT("Cast<%s>"), Cast->TargetType ? *Cast->TargetType->GetName() : TEXT("?"));
		}
		FString Title = FirstLine(Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		if (Node->IsA<UK2Node_Event>() && Title.StartsWith(TEXT("Event ")))
		{
			Title.RightChopInline(6);
		}
		return Title.IsEmpty() ? Node->GetClass()->GetName() : Title;
	}

	/** True when the pin's value is worth printing as an argument. */
	static bool ShowsArgument(const UEdGraphPin* Pin)
	{
		if (Pin->LinkedTo.Num() > 0)
		{
			return true;
		}
		if (Pin->SubPins.Num() > 0)
		{
			for (const UEdGraphPin* Sub : Pin->SubPins)
			{
				if (Sub && ShowsArgument(Sub))
				{
					return true;
				}
			}
			return false;
		}
		if (Pin->bHidden || Pin->PinName == UEdGraphSchema_K2::PN_Self || Pin->PinName == TEXT("WorldContextObject"))
		{
			return false;
		}
		if (Pin->DoesDefaultValueMatchAutogenerated())
		{
			return false;
		}
		// A cleared string/text is a value of its own; other types have nothing to print when empty.
		const FName Category = Pin->PinType.PinCategory;
		return Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Text
			|| Pin->DefaultObject || !Pin->DefaultValue.IsEmpty() || !Pin->DefaultTextValue.IsEmpty();
	}

	static FString Literal(const UEdGraphPin* Pin)
	{
		const FName Category = Pin->PinType.PinCategory;
		if (Pin->DefaultObject)
		{
			return Pin->DefaultObject->GetName();
		}
		if (Category == UEdGraphSchema_K2::PC_Text)
		{
			return Quote(Pin->DefaultTextValue.ToString());
		}
		if (Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Name)
		{
			return Quote(Pin->DefaultValue);
		}
		if (const UEnum* Enum = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()))
		{
			return FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *Pin->DefaultValue);
		}
		return Pin->DefaultValue.IsEmpty() ? FString(TEXT("None")) : Pin->DefaultValue;
	}

	/** "Vector_X" under parent "Vector" -> "X". */
	static FString SubPinSuffix(const UEdGraphPin* Sub)
	{
		const FString Name = Sub->PinName.ToString();
		if (Sub->ParentPin)
		{
			const FString Prefix = Sub->ParentPin->PinName.ToString() + TEXT("_");
			if (Name.StartsWith(Prefix))
			{
				return Name.Mid(Prefix.Len());
			}
		}
		return Name;
	}

	/** Per-graph rendering state. */
	class FGraphRender
	{
	public:
		FGraphRender(UEdGraph* InGraph, const FMonolithBlueprintGraphRenderer::FOptions& InOptions)
			: Graph(InGraph), Options(InOptions)
		{
		}

		void Run(TArray<FString>& Lines)
		{
			Out = &Lines;
			CollectComments();

			bool bAnyExec = false;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !IsComment(Node) && !IsKnot(Node) && HasExecPins(Node))
				{
					bAnyExec = true;
					break;
				}
			}

			if (bAnyExec)
			{
				CountExecIncoming();
				RenderExecFlow();
			}
			else
			{
				RenderSinks();
			}
			RenderLeftoverComments();
		}

	private:
		UEdGraph* Graph;
		const FMonolithBlueprintGraphRenderer::FOptions& Options;
		TArray<FString>* Out = nullptr;

		TMap<const UEdGraphNode*, int32> ExecIncoming;
		TMap<const UEdGraphNode*, FString> Labels;
		TSet<const UEdGraphNode*> Visited;
		TMap<const UEdGraphNode*, FString> Handles;
		int32 NextLabel = 1;
		int32 NextHandle = 1;

		struct FCommentBox
		{
			const UEdGraphNode_Comment* Node = nullptr;
			FBox2D Rect;
			bool bPrinted = false;
		};
		TArray<FCommentBox> Comments;

		void Emit(int32 Indent, const FString& Text)
		{
			Out->Add(FString::ChrN(Indent * 2, TEXT(' ')) + Text);
		}

		// ---------------------------------------------------------------- comments
		void CollectComments()
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
				{
					FCommentBox& Box = Comments.AddDefaulted_GetRef();
					Box.Node = Comment;
					Box.Rect = FBox2D(FVector2D(Comment->NodePosX, Comment->NodePosY),
						FVector2D(Comment->NodePosX + Comment->NodeWidth, Comment->NodePosY + Comment->NodeHeight));
				}
			}
		}

		void EmitCommentsFor(const UEdGraphNode* Node, int32 Indent)
		{
			const FVector2D Pos(Node->NodePosX, Node->NodePosY);
			for (FCommentBox& Box : Comments)
			{
				if (!Box.bPrinted && Box.Rect.IsInside(Pos))
				{
					Box.bPrinted = true;
					Emit(Indent, FString::Printf(TEXT("# [%s]"), *OneLine(Box.Node->NodeComment)));
				}
			}
		}

		void RenderLeftoverComments()
		{
			bool bHeader = false;
			for (const FCommentBox& Box : Comments)
			{
				if (!Box.bPrinted)
				{
					if (!bHeader)
					{
						Out->Add(TEXT(""));
						Emit(0, TEXT("# comment boxes enclosing no rendered statement:"));
						bHeader = true;
					}
					Emit(0, FString::Printf(TEXT("# [%s]"), *OneLine(Box.Node->NodeComment)));
				}
			}
		}

		// ---------------------------------------------------------------- expressions
		/** Inline (expression) nodes: pure nodes, and nodes with no exec pins at all (anim graph nodes). */
		static bool IsInline(const UEdGraphNode* Node)
		{
			if (const UK2Node* K2 = Cast<UK2Node>(Node))
			{
				if (K2->IsNodePure())
				{
					return true;
				}
			}
			return !HasExecPins(Node);
		}

		FString HandleFor(const UEdGraphNode* Node)
		{
			if (const FString* Existing = Handles.Find(Node))
			{
				return *Existing;
			}
			return Handles.Add(Node, FString::Printf(TEXT("h%d"), NextHandle++));
		}

		FString TargetPrefix(const UEdGraphNode* Node, int32 Depth)
		{
			const UEdGraphPin* Self = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
			if (Self && Self->LinkedTo.Num() > 0)
			{
				return InputExpr(Self, Depth) + TEXT(".");
			}
			return FString();
		}

		FString Args(const UEdGraphNode* Node, int32 Depth)
		{
			TArray<FString> Parts;
			for (const UEdGraphPin* Pin : DataPins(Node, EGPD_Input))
			{
				if (Pin->PinName == UEdGraphSchema_K2::PN_Self || !ShowsArgument(Pin))
				{
					continue;
				}
				Parts.Add(FString::Printf(TEXT("%s=%s"), *Pin->PinName.ToString(), *InputExpr(Pin, Depth)));
			}
			return FString::Join(Parts, TEXT(", "));
		}

		FString InputExpr(const UEdGraphPin* Pin, int32 Depth)
		{
			if (Pin->LinkedTo.Num() > 0)
			{
				const UEdGraphPin* Source = ResolveSource(Pin);
				return Source ? OutputExpr(Source, Depth) : FString(TEXT("?"));
			}
			if (Pin->SubPins.Num() > 0)
			{
				TArray<FString> Parts;
				for (const UEdGraphPin* Sub : Pin->SubPins)
				{
					if (Sub && ShowsArgument(Sub))
					{
						Parts.Add(FString::Printf(TEXT("%s=%s"), *SubPinSuffix(Sub), *InputExpr(Sub, Depth)));
					}
				}
				return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(", ")));
			}
			return Literal(Pin);
		}

		FString OutputExpr(const UEdGraphPin* Pin, int32 Depth)
		{
			// A split output sub-pin reads a field of its parent's value.
			if (Pin->ParentPin)
			{
				return OutputExpr(Pin->ParentPin, Depth) + TEXT(".") + SubPinSuffix(Pin);
			}
			const UEdGraphNode* Node = Pin->GetOwningNode();
			if (IsEntry(Node))
			{
				// The event's own delegate pin stands for the event; the rest are its parameters.
				return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate ? NodeName(Node) : Pin->PinName.ToString();
			}
			if (!IsInline(Node))
			{
				const FString Handle = HandleFor(Node);
				return DataPins(Node, EGPD_Output).Num() > 1 ? Handle + TEXT(".") + Pin->PinName.ToString() : Handle;
			}
			if (Depth >= Options.MaxExprDepth)
			{
				return TEXT("...");
			}
			return InlineExpr(Node, Pin, Depth + 1);
		}

		FString InlineExpr(const UEdGraphNode* Node, const UEdGraphPin* OutPin, int32 Depth)
		{
			if (Node->IsA<UK2Node_Self>())
			{
				return TEXT("self");
			}
			if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node))
			{
				return TargetPrefix(Node, Depth) + Get->GetVarNameString();
			}
			if (Node->IsA<UK2Node_BreakStruct>())
			{
				const TArray<UEdGraphPin*> Inputs = DataPins(Node, EGPD_Input);
				const FString Struct = Inputs.Num() > 0 ? InputExpr(Inputs[0], Depth) : FString(TEXT("?"));
				return Struct + TEXT(".") + OutPin->PinName.ToString();
			}
			if (Node->IsA<UK2Node_GetArrayItem>())
			{
				const TArray<UEdGraphPin*> Inputs = DataPins(Node, EGPD_Input);
				if (Inputs.Num() >= 2)
				{
					return FString::Printf(TEXT("%s[%s]"), *InputExpr(Inputs[0], Depth), *InputExpr(Inputs[1], Depth));
				}
			}
			if (Node->IsA<UK2Node_MakeArray>())
			{
				TArray<FString> Items;
				for (const UEdGraphPin* Pin : DataPins(Node, EGPD_Input))
				{
					Items.Add(InputExpr(Pin, Depth));
				}
				return FString::Printf(TEXT("[%s]"), *FString::Join(Items, TEXT(", ")));
			}
			if (Node->IsA<UK2Node_MakeMap>())
			{
				const TArray<UEdGraphPin*> Inputs = DataPins(Node, EGPD_Input);
				TArray<FString> Items;
				for (int32 Index = 0; Index + 1 < Inputs.Num(); Index += 2)
				{
					Items.Add(FString::Printf(TEXT("%s: %s"), *InputExpr(Inputs[Index], Depth), *InputExpr(Inputs[Index + 1], Depth)));
				}
				return FString::Printf(TEXT("{%s}"), *FString::Join(Items, TEXT(", ")));
			}
			if (const UK2Node_EnumLiteral* EnumLiteral = Cast<UK2Node_EnumLiteral>(Node))
			{
				const UEdGraphPin* EnumPin = Node->FindPin(UK2Node_EnumLiteral::GetEnumInputPinName());
				const FString Value = EnumPin ? EnumPin->DefaultValue : FString(TEXT("?"));
				return FString::Printf(TEXT("%s::%s"), EnumLiteral->Enum ? *EnumLiteral->Enum->GetName() : TEXT("?"), *Value);
			}
			if (const UK2Node_Literal* LiteralNode = Cast<UK2Node_Literal>(Node))
			{
				const UObject* Object = LiteralNode->GetObjectRef();
				return Object ? Object->GetName() : FString(TEXT("None"));
			}

			// Generic pure call / struct maker: Name(args), plus .Pin when several outputs. A non-call
			// node with nothing to pass (a macro local, a getter node) is just its title.
			const FString NodeArgs = Args(Node, Depth);
			const FString Call = (NodeArgs.IsEmpty() && !Node->IsA<UK2Node_CallFunction>())
				? TargetPrefix(Node, Depth) + NodeName(Node)
				: FString::Printf(TEXT("%s%s(%s)"), *TargetPrefix(Node, Depth), *NodeName(Node), *NodeArgs);
			return DataPins(Node, EGPD_Output).Num() > 1 ? Call + TEXT(".") + OutPin->PinName.ToString() : Call;
		}

		// ---------------------------------------------------------------- statements
		FString Decorate(const UEdGraphNode* Node, const FString& Text) const
		{
			FString Result = Text;
			if (Node->GetDesiredEnabledState() == ENodeEnabledState::Disabled)
			{
				Result += TEXT(" [DISABLED]");
			}
			if (Options.bPositions)
			{
				Result += FString::Printf(TEXT(" @(%d,%d)"), Node->NodePosX, Node->NodePosY);
			}
			if (!Node->NodeComment.IsEmpty())
			{
				Result += TEXT("  // ") + OneLine(Node->NodeComment);
			}
			return Result;
		}

		/** " [in: Pin]" for a node with several exec inputs (Gate.Open, Timeline.Reverse, ...). */
		static FString EnteredTag(const UEdGraphNode* Node, const UEdGraphPin* EnteredBy)
		{
			if (EnteredBy && !IsTunnelBoundary(Node) && ExecPins(Node, EGPD_Input).Num() > 1)
			{
				return FString::Printf(TEXT(" [in: %s]"), *EnteredBy->GetDisplayName().ToString());
			}
			return FString();
		}

		FString Statement(const UEdGraphNode* Node, const UEdGraphPin* EnteredBy)
		{
			FString Text;
			if (const UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(Node))
			{
				const UEdGraphPin* Value = Node->FindPin(Set->GetVarName(), EGPD_Input);
				Text = FString::Printf(TEXT("%s%s = %s"), *TargetPrefix(Node, 0), *Set->GetVarNameString(),
					Value ? *InputExpr(Value, 0) : TEXT("?"));
			}
			else if (Node->IsA<UK2Node_FunctionResult>())
			{
				const FString Values = Args(Node, 0);
				Text = Values.IsEmpty() ? FString(TEXT("return")) : FString::Printf(TEXT("return (%s)"), *Values);
			}
			else if (Node->IsA<UK2Node_IfThenElse>())
			{
				const UEdGraphPin* Condition = Node->FindPin(UEdGraphSchema_K2::PN_Condition, EGPD_Input);
				Text = FString::Printf(TEXT("if %s"), Condition ? *InputExpr(Condition, 0) : TEXT("?"));
			}
			else if (IsTunnelBoundary(Node))
			{
				// A macro's output tunnel: the exit it was reached through.
				const FString Values = Args(Node, 0);
				Text = FString::Printf(TEXT("exit %s(%s)"), EnteredBy ? *EnteredBy->PinName.ToString() : TEXT(""), *Values);
			}
			else
			{
				Text = FString::Printf(TEXT("%s%s(%s)"), *TargetPrefix(Node, 0), *NodeName(Node), *Args(Node, 0));
			}

			Text += EnteredTag(Node, EnteredBy);

			// Used data outputs get a handle the later expressions refer to.
			bool bOutputsUsed = false;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && !IsExec(Pin) && Pin->LinkedTo.Num() > 0)
				{
					bOutputsUsed = true;
					break;
				}
			}
			if (bOutputsUsed)
			{
				Text = HandleFor(Node) + TEXT(" = ") + Text;
			}
			return Decorate(Node, Text);
		}

		// ---------------------------------------------------------------- exec flow
		void CountExecIncoming()
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || IsKnot(Node) || IsComment(Node))
				{
					continue;
				}
				for (const UEdGraphPin* Pin : ExecPins(Node, EGPD_Output))
				{
					TArray<UEdGraphPin*> Targets;
					ResolveTargets(Pin, Targets);
					for (const UEdGraphPin* Target : Targets)
					{
						ExecIncoming.FindOrAdd(Target->GetOwningNode())++;
					}
				}
			}
		}

		static bool IsEntry(const UEdGraphNode* Node)
		{
			return !IsKnot(Node) && !IsComment(Node) && ExecPins(Node, EGPD_Output).Num() > 0 && ExecPins(Node, EGPD_Input).Num() == 0;
		}

		/** Return nodes and macro exits: printed on every path that reaches them, never labelled. */
		static bool IsTerminal(const UEdGraphNode* Node)
		{
			return Node->IsA<UK2Node_FunctionResult>() || (IsTunnelBoundary(Node) && ExecPins(Node, EGPD_Input).Num() > 0);
		}

		FString EntryHeader(const UEdGraphNode* Node)
		{
			TArray<FString> Params;
			for (const UEdGraphPin* Pin : DataPins(Node, EGPD_Output))
			{
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
				{
					continue; // the event's own "OutputDelegate" pin
				}
				Params.Add(FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *PinTypeText(Pin)));
			}
			const FString Name = (Node->IsA<UK2Node_FunctionEntry>() || IsTunnelBoundary(Node)) ? FString(TEXT("entry")) : NodeName(Node);
			return Decorate(Node, FString::Printf(TEXT("%s(%s):"), *Name, *FString::Join(Params, TEXT(", "))));
		}

		void RenderExecFlow()
		{
			TArray<UEdGraphNode*> Entries;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && IsEntry(Node))
				{
					Entries.Add(Node);
				}
			}
			Entries.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
			{
				const bool bAFunction = A.IsA<UK2Node_FunctionEntry>() || IsTunnelBoundary(&A);
				const bool bBFunction = B.IsA<UK2Node_FunctionEntry>() || IsTunnelBoundary(&B);
				if (bAFunction != bBFunction)
				{
					return bAFunction;
				}
				return A.NodePosY != B.NodePosY ? A.NodePosY < B.NodePosY : A.NodePosX < B.NodePosX;
			});

			for (UEdGraphNode* Entry : Entries)
			{
				Out->Add(TEXT(""));
				EmitCommentsFor(Entry, 0);
				Emit(0, EntryHeader(Entry));
				Visited.Add(Entry);
				FollowExecOutputs(Entry, 1);
			}

			// Dead code: exec nodes no entry reaches. Walk the chain heads first, then whatever
			// is left (cycles).
			TArray<UEdGraphNode*> Unreached;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !IsKnot(Node) && !IsComment(Node) && HasExecPins(Node) && !IsInline(Node) && !Visited.Contains(Node))
				{
					Unreached.Add(Node);
				}
			}
			if (Unreached.Num() > 0)
			{
				Out->Add(TEXT(""));
				Emit(0, FString::Printf(TEXT("# not reachable from any entry (%d exec node(s)):"), Unreached.Num()));
				Unreached.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
				{
					return A.NodePosY != B.NodePosY ? A.NodePosY < B.NodePosY : A.NodePosX < B.NodePosX;
				});
				for (UEdGraphNode* Node : Unreached)
				{
					if (!Visited.Contains(Node) && ExecIncoming.FindRef(Node) == 0)
					{
						Walk(Node, nullptr, 1);
					}
				}
				for (UEdGraphNode* Node : Unreached)
				{
					if (!Visited.Contains(Node))
					{
						// Starting here is one more way in, on top of the links that come back to it.
						ExecIncoming.FindOrAdd(Node)++;
						Walk(Node, nullptr, 1);
					}
				}
			}
		}

		void Walk(const UEdGraphNode* Node, const UEdGraphPin* EnteredBy, int32 Indent)
		{
			if (IsTerminal(Node))
			{
				Visited.Add(Node);
				EmitCommentsFor(Node, Indent);
				Emit(Indent, Statement(Node, EnteredBy));
				return;
			}
			if (Visited.Contains(Node))
			{
				const FString* Label = Labels.Find(Node);
				Emit(Indent, FString::Printf(TEXT("goto %s (%s)%s"), Label ? **Label : TEXT("?"), *NodeName(Node), *EnteredTag(Node, EnteredBy)));
				return;
			}
			Visited.Add(Node);
			EmitCommentsFor(Node, Indent);

			FString Line = Statement(Node, EnteredBy);
			if (ExecIncoming.FindRef(Node) > 1)
			{
				const FString Label = Labels.Add(Node, FString::Printf(TEXT("L%d"), NextLabel++));
				Line = Label + TEXT(": ") + Line;
			}
			Emit(Indent, Line);
			FollowExecOutputs(Node, Indent);
		}

		void FollowExecOutputs(const UEdGraphNode* Node, int32 Indent)
		{
			const TArray<UEdGraphPin*> Outputs = ExecPins(Node, EGPD_Output);
			const bool bBranching = Outputs.Num() > 1;
			for (const UEdGraphPin* Pin : Outputs)
			{
				TArray<UEdGraphPin*> Targets;
				ResolveTargets(Pin, Targets);
				if (Targets.Num() == 0)
				{
					continue;
				}
				if (bBranching)
				{
					FString Label = Pin->GetDisplayName().ToString();
					if (Node->IsA<UK2Node_IfThenElse>())
					{
						Label = Pin->PinName == UEdGraphSchema_K2::PN_Then ? TEXT("true") : TEXT("false");
					}
					if (Label.IsEmpty())
					{
						Label = Pin->PinName.ToString();
					}
					Emit(Indent + 1, Label + TEXT(":"));
					for (const UEdGraphPin* Target : Targets)
					{
						Walk(Target->GetOwningNode(), Target, Indent + 2);
					}
				}
				else
				{
					for (const UEdGraphPin* Target : Targets)
					{
						Walk(Target->GetOwningNode(), Target, Indent);
					}
				}
			}
		}

		// ---------------------------------------------------------------- data-only graphs
		void RenderSinks()
		{
			TArray<UEdGraphNode*> Sinks;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || IsKnot(Node) || IsComment(Node))
				{
					continue;
				}
				bool bOutputLinked = false;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
					{
						bOutputLinked = true;
						break;
					}
				}
				if (!bOutputLinked)
				{
					Sinks.Add(Node);
				}
			}
			Sinks.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
			{
				return A.NodePosY != B.NodePosY ? A.NodePosY < B.NodePosY : A.NodePosX < B.NodePosX;
			});
			for (UEdGraphNode* Sink : Sinks)
			{
				Out->Add(TEXT(""));
				EmitCommentsFor(Sink, 0);
				Emit(0, Decorate(Sink, FString::Printf(TEXT("%s <- (%s)"), *NodeName(Sink), *Args(Sink, 0))));
			}
		}
	};
}

TArray<UEdGraph*> FMonolithBlueprintGraphRenderer::CollectGraphs(UBlueprint* Blueprint)
{
	TArray<UEdGraph*> Ordered;
	if (!Blueprint)
	{
		return Ordered;
	}
	auto AddAll = [&Ordered](const TArray<TObjectPtr<UEdGraph>>& Graphs)
	{
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph)
			{
				Ordered.AddUnique(Graph);
			}
		}
	};
	AddAll(Blueprint->UbergraphPages);
	AddAll(Blueprint->FunctionGraphs);
	AddAll(Blueprint->MacroGraphs);
	AddAll(Blueprint->DelegateSignatureGraphs);
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		AddAll(Interface.Graphs);
	}
	// Nested graphs: collapsed graphs, state machines, anim graphs and their states/rules.
	TArray<UEdGraph*> All;
	Blueprint->GetAllGraphs(All);
	for (UEdGraph* Graph : All)
	{
		if (Graph)
		{
			Ordered.AddUnique(Graph);
		}
	}
	return Ordered;
}

FString FMonolithBlueprintGraphRenderer::DescribeGraph(UBlueprint* Blueprint, UEdGraph* Graph, FString& OutType)
{
	using namespace MonolithGraphRenderDetail;

	FString InterfaceName;
	FString ParentGraph;
	OutType = MonolithBlueprintInternal::ClassifyGraphType(Blueprint, Graph, InterfaceName, &ParentGraph);

	if (OutType == TEXT("function") || OutType == TEXT("interface"))
	{
		// Signature from the entry's outputs and the result's inputs.
		TArray<FString> In, OutParams;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->IsA<UK2Node_FunctionEntry>())
			{
				for (const UEdGraphPin* Pin : DataPins(Node, EGPD_Output))
				{
					In.Add(FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *PinTypeText(Pin)));
				}
			}
			else if (Node && Node->IsA<UK2Node_FunctionResult>() && OutParams.Num() == 0)
			{
				for (const UEdGraphPin* Pin : DataPins(Node, EGPD_Input))
				{
					OutParams.Add(FString::Printf(TEXT("%s: %s"), *Pin->PinName.ToString(), *PinTypeText(Pin)));
				}
			}
		}
		const FString Prefix = OutType == TEXT("interface") && !InterfaceName.IsEmpty()
			? FString::Printf(TEXT("interface %s."), *InterfaceName) : FString(TEXT("function "));
		FString Header = FString::Printf(TEXT("== %s%s(%s)"), *Prefix, *Graph->GetName(), *FString::Join(In, TEXT(", ")));
		if (OutParams.Num() > 0)
		{
			Header += FString::Printf(TEXT(" -> (%s)"), *FString::Join(OutParams, TEXT(", ")));
		}
		return Header + TEXT(" ==");
	}
	if (OutType == TEXT("event_graph"))
	{
		return FString::Printf(TEXT("== event graph %s =="), *Graph->GetName());
	}
	if (OutType == TEXT("subgraph"))
	{
		return FString::Printf(TEXT("== subgraph %s (in %s) =="), *Graph->GetName(), *ParentGraph);
	}
	return FString::Printf(TEXT("== %s %s =="), *OutType.Replace(TEXT("_"), TEXT(" ")), *Graph->GetName());
}

FString FMonolithBlueprintGraphRenderer::RenderGraph(UBlueprint* Blueprint, UEdGraph* Graph, const FOptions& Options, FString& OutType)
{
	TArray<FString> Lines;
	Lines.Add(DescribeGraph(Blueprint, Graph, OutType));
	MonolithGraphRenderDetail::FGraphRender Render(Graph, Options);
	Render.Run(Lines);
	return FString::Join(Lines, TEXT("\n"));
}
