// Copyright 1998-2017 Epic Games, Inc. All Rights Reserved.

#include "DisplayNodes/SequencerTrackNode.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SBox.h"
#include "DisplayNodes/SequencerObjectBindingNode.h"
#include "Framework/Commands/UIAction.h"
#include "Textures/SlateIcon.h"
#include "UObject/UnrealType.h"
#include "MovieSceneTrack.h"
#include "SSequencer.h"
#include "MovieSceneNameableTrack.h"
#include "ISequencerTrackEditor.h"
#include "ScopedTransaction.h"
#include "SKeyNavigationButtons.h"

namespace SequencerNodeConstants
{
	extern const float CommonPadding;
}


/* FTrackNode structors
 *****************************************************************************/

FSequencerTrackNode::FSequencerTrackNode(UMovieSceneTrack& InAssociatedTrack, ISequencerTrackEditor& InAssociatedEditor, bool bInCanBeDragged, TSharedPtr<FSequencerDisplayNode> InParentNode, FSequencerNodeTree& InParentTree)
	: FSequencerDisplayNode(InAssociatedTrack.GetTrackName(), InParentNode, InParentTree)
	, AssociatedEditor(InAssociatedEditor)
	, AssociatedTrack(&InAssociatedTrack)
	, bCanBeDragged(bInCanBeDragged)
{ }


/* FTrackNode interface
 *****************************************************************************/

void FSequencerTrackNode::SetSectionAsKeyArea(TSharedRef<IKeyArea>& KeyArea)
{
	if( !TopLevelKeyNode.IsValid() )
	{
		bool bTopLevel = true;
		TopLevelKeyNode = MakeShareable( new FSequencerSectionKeyAreaNode( GetNodeName(), FText::GetEmpty(), nullptr, ParentTree, bTopLevel ) );
	}

	TopLevelKeyNode->AddKeyArea( KeyArea );
}

void FSequencerTrackNode::AddKey(const FGuid& ObjectGuid)
{
	AssociatedEditor.AddKey(ObjectGuid);
}


int32 FSequencerTrackNode::GetMaxRowIndex() const
{
	int32 MaxRowIndex = 0;

	for (int32 i = 0; i < Sections.Num(); ++i)
	{
		MaxRowIndex = FMath::Max(MaxRowIndex, Sections[i]->GetSectionObject()->GetRowIndex());
	}

	return MaxRowIndex;
}


void FSequencerTrackNode::FixRowIndices()
{
	if (AssociatedTrack->SupportsMultipleRows())
	{
		// remove any empty track rows by waterfalling down sections to be as compact as possible
		TArray< TArray< TSharedRef<ISequencerSection> > > TrackIndices;
		TrackIndices.AddZeroed(GetMaxRowIndex() + 1);
		for (int32 i = 0; i < Sections.Num(); ++i)
		{
			TrackIndices[Sections[i]->GetSectionObject()->GetRowIndex()].Add(Sections[i]);
		}

		int32 NewIndex = 0;

		for (int32 i = 0; i < TrackIndices.Num(); ++i)
		{
			const TArray< TSharedRef<ISequencerSection> >& SectionsForThisIndex = TrackIndices[i];
			if (SectionsForThisIndex.Num() > 0)
			{
				for (int32 j = 0; j < SectionsForThisIndex.Num(); ++j)
				{
					SectionsForThisIndex[j]->GetSectionObject()->SetRowIndex(NewIndex);
				}

				++NewIndex;
			}
		}
	}
	else
	{
		// non master tracks can only have a single row
		for (int32 i = 0; i < Sections.Num(); ++i)
		{
			Sections[i]->GetSectionObject()->SetRowIndex(0);
		}
	}
}


/* FSequencerDisplayNode interface
 *****************************************************************************/

namespace
{
	void AddBoolPropertyMenuItem(FMenuBuilder& MenuBuilder, FCanExecuteAction InCanExecute, UMovieSceneTrack* Track, const UBoolProperty* Property, void* PropertyContainer)
	{
		MenuBuilder.AddMenuEntry(
			Property->GetDisplayNameText(),
			Property->GetToolTipText(),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Track, Property, PropertyContainer]{
					FScopedTransaction Transaction(FText::Format(NSLOCTEXT("Sequencer", "TrackNodeSetRoundEvaluation", "Set '{0}'"), Property->GetDisplayNameText()));
					Track->Modify();

					bool bIsSet = Property->GetPropertyValue(PropertyContainer);
					Property->SetPropertyValue(PropertyContainer, !bIsSet);
				}),
				InCanExecute,
				FIsActionChecked::CreateLambda([PropertyContainer, Property]{ return Property->GetPropertyValue(PropertyContainer); })
			),
			NAME_None,
			EUserInterfaceActionType::Check
		);
	}
}

void FSequencerTrackNode::BuildContextMenu(FMenuBuilder& MenuBuilder)
{
	AssociatedEditor.BuildTrackContextMenu(MenuBuilder, AssociatedTrack.Get());
	FSequencerDisplayNode::BuildContextMenu(MenuBuilder );

	MenuBuilder.BeginSection("GeneralTrackOptions", NSLOCTEXT("Sequencer", "TrackNodeGeneralOptions", "Track Options"));
	{
		UMovieSceneTrack* Track = AssociatedTrack.Get();
		if (!Track)
		{
			return;
		}

		bool bIsReadOnly = !GetSequencer().IsReadOnly();
		FCanExecuteAction CanExecute = FCanExecuteAction::CreateLambda([bIsReadOnly]{ return bIsReadOnly; });

		UStruct* EvalOptionsStruct = FMovieSceneTrackEvalOptions::StaticStruct();

		const UBoolProperty* NearestSectionProperty = Cast<UBoolProperty>(EvalOptionsStruct->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FMovieSceneTrackEvalOptions, bEvaluateNearestSection)));
		if (NearestSectionProperty && Track->EvalOptions.bCanEvaluateNearestSection)
		{
			AddBoolPropertyMenuItem(MenuBuilder, CanExecute, Track, NearestSectionProperty, NearestSectionProperty->ContainerPtrToValuePtr<void>(&Track->EvalOptions));
		}

		const UBoolProperty* PrerollProperty = Cast<UBoolProperty>(EvalOptionsStruct->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FMovieSceneTrackEvalOptions, bEvaluateInPreroll)));
		if (PrerollProperty)
		{
			AddBoolPropertyMenuItem(MenuBuilder, CanExecute, Track, PrerollProperty, PrerollProperty->ContainerPtrToValuePtr<void>(&Track->EvalOptions));
		}

		const UBoolProperty* PostrollProperty = Cast<UBoolProperty>(EvalOptionsStruct->FindPropertyByName(GET_MEMBER_NAME_CHECKED(FMovieSceneTrackEvalOptions, bEvaluateInPostroll)));
		if (PostrollProperty)
		{
			AddBoolPropertyMenuItem(MenuBuilder, CanExecute, Track, PostrollProperty, PostrollProperty->ContainerPtrToValuePtr<void>(&Track->EvalOptions));
		}
	}
	MenuBuilder.EndSection();
}


bool FSequencerTrackNode::CanRenameNode() const
{
	auto NameableTrack = Cast<UMovieSceneNameableTrack>(AssociatedTrack.Get());

	if (NameableTrack != nullptr)
	{
		return NameableTrack->CanRename();
	}
	return false;
}

TSharedRef<SWidget> FSequencerTrackNode::GetCustomOutlinerContent()
{
	TSharedPtr<FSequencerSectionKeyAreaNode> KeyAreaNode = GetTopLevelKeyNode();
	if (KeyAreaNode.IsValid())
	{
		// @todo - Sequencer - Support multiple sections/key areas?
		TArray<TSharedRef<IKeyArea>> KeyAreas = KeyAreaNode->GetAllKeyAreas();

		if (KeyAreas.Num() > 0)
		{
			// Create the widgets for the key editor and key navigation buttons
			TSharedRef<SHorizontalBox> BoxPanel = SNew(SHorizontalBox);

			if (KeyAreas[0]->CanCreateKeyEditor())
			{
				BoxPanel->AddSlot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(100)
					.HAlign(HAlign_Left)
					.IsEnabled(!GetSequencer().IsReadOnly())
					[
						KeyAreas[0]->CreateKeyEditor(&GetSequencer())
					]
				];
			}
			else
			{
				BoxPanel->AddSlot()
				[
					SNew(SSpacer)
				];
			}

			BoxPanel->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SKeyNavigationButtons, AsShared())
			];

			return SNew(SBox)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				[
					BoxPanel
				];
		}
	}
	else
	{
		FGuid ObjectBinding;
		TSharedPtr<FSequencerDisplayNode> ParentSeqNode = GetParent();

		if (ParentSeqNode.IsValid() && (ParentSeqNode->GetType() == ESequencerNode::Object))
		{
			ObjectBinding = StaticCastSharedPtr<FSequencerObjectBindingNode>(ParentSeqNode)->GetObjectBinding();
		}
		FBuildEditWidgetParams Params;
		Params.NodeIsHovered = TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateSP(this, &FSequencerDisplayNode::IsHovered));

		TSharedPtr<SWidget> Widget = GetSequencer().IsReadOnly() ? SNullWidget::NullWidget : AssociatedEditor.BuildOutlinerEditWidget(ObjectBinding, AssociatedTrack.Get(), Params);

		TSharedRef<SHorizontalBox> BoxPanel = SNew(SHorizontalBox);

		bool bHasKeyableAreas = false;

		TArray<TSharedRef<FSequencerSectionKeyAreaNode>> ChildKeyAreaNodes;
		FSequencerDisplayNode::GetChildKeyAreaNodesRecursively(ChildKeyAreaNodes);
		for (int32 ChildIndex = 0; ChildIndex < ChildKeyAreaNodes.Num() && !bHasKeyableAreas; ++ChildIndex)
		{
			TArray< TSharedRef<IKeyArea> > ChildKeyAreas = ChildKeyAreaNodes[ChildIndex]->GetAllKeyAreas();

			for (int32 ChildKeyAreaIndex = 0; ChildKeyAreaIndex < ChildKeyAreas.Num() && !bHasKeyableAreas; ++ChildKeyAreaIndex)
			{
				if (ChildKeyAreas[ChildKeyAreaIndex]->CanCreateKeyEditor())
				{
					bHasKeyableAreas = true;
				}
			}
		}

		if (Widget.IsValid())
		{
			BoxPanel->AddSlot()
			.VAlign(VAlign_Center)
			.HAlign(HAlign_Right)
			[
				Widget.ToSharedRef()
			];
		}

		if (bHasKeyableAreas)
		{
			BoxPanel->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SKeyNavigationButtons, AsShared())
			];
		}

		return SNew(SBox)
			.VAlign(VAlign_Center)
			.HAlign(HAlign_Right)
			[
				BoxPanel
			];
	}


	return SNew(SSpacer);
}


const FSlateBrush* FSequencerTrackNode::GetIconBrush() const
{
	return AssociatedEditor.GetIconBrush();
}


bool FSequencerTrackNode::CanDrag() const
{
	return bCanBeDragged;
}


void FSequencerTrackNode::GetChildKeyAreaNodesRecursively(TArray<TSharedRef<FSequencerSectionKeyAreaNode>>& OutNodes) const
{
	FSequencerDisplayNode::GetChildKeyAreaNodesRecursively(OutNodes);

	if (TopLevelKeyNode.IsValid())
	{
		OutNodes.Add(TopLevelKeyNode.ToSharedRef());
	}
}


FText FSequencerTrackNode::GetDisplayName() const
{
	return AssociatedTrack.IsValid() ? AssociatedTrack->GetDisplayName() : FText::GetEmpty();
}


float FSequencerTrackNode::GetNodeHeight() const
{
	if (Sections.Num() > 0)
	{
		return (Sections[0]->GetSectionHeight() + 2 * SequencerNodeConstants::CommonPadding) * (GetMaxRowIndex() + 1);
	}

	return SequencerLayoutConstants::SectionAreaDefaultHeight + 2*SequencerNodeConstants::CommonPadding;
}


FNodePadding FSequencerTrackNode::GetNodePadding() const
{
	return FNodePadding(0.f);
}


ESequencerNode::Type FSequencerTrackNode::GetType() const
{
	return ESequencerNode::Track;
}


void FSequencerTrackNode::SetDisplayName(const FText& NewDisplayName)
{
	auto NameableTrack = Cast<UMovieSceneNameableTrack>(AssociatedTrack.Get());

	if (NameableTrack != nullptr)
	{
		NameableTrack->SetDisplayName(NewDisplayName);
	}
}
