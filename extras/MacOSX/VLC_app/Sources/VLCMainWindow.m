/*****************************************************************************
 * VLCMainWindow.m: VLCMainWindow implementation
 *****************************************************************************
 * Copyright (C) 2007 Pierre d'Herbemont
 * Copyright (C) 2007 the VideoLAN team
 * $Id$
 *
 * Authors: Pierre d'Herbemont <pdherbemont # videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#import "VLCMainWindow.h"
#import "ImageAndTextCell.h"
#import "VLCMediaArrayController.h"
#import "VLCBrowsableVideoView.h"
#import "VLCAppAdditions.h"

/******************************************************************************
 * VLCMainWindow (CategoriesListDelegate)
 */
@implementation VLCMainWindow (CategoriesListDelegate)
- (BOOL)outlineView:(NSOutlineView *)outlineView isGroupItem:(id)item
{
    return [[item representedObject] isKindOfClass:[NSDictionary class]];
}
- (BOOL)outlineView:(NSOutlineView *)outlineView shouldSelectItem:(id)item
{
    return !([[item representedObject] isKindOfClass:[NSDictionary class]]);
}
- (void)outlineView:(NSOutlineView *)outlineView willDisplayCell:(id)cell forTableColumn:(NSTableColumn *)tableColumn item:(id)item
{
    [cell setRepresentedObject:[item representedObject]];
}
@end

/******************************************************************************
 * VLCMainWindow (CategoriesListDataSource)
 */
@implementation VLCMainWindow (CategoriesListDataSource)
/* Drag and drop */
- (BOOL)outlineView:(NSOutlineView *)outlineView acceptDrop:(id < NSDraggingInfo >)info item:(id)item childIndex:(NSInteger)index
{
    int i;

    if(![item respondsToSelector:@selector(representedObject)])
        return NO;
    
    NSArray *droppedItems = [[info draggingPasteboard] propertyListForType:@"VLCMediaURLType"];
    if( !droppedItems )
        droppedItems = [[info draggingPasteboard] propertyListForType:NSFilenamesPboardType];
    if( !droppedItems )
        droppedItems = [[info draggingPasteboard] propertyListForType:NSURLPboardType];

    NSAssert( droppedItems, @"Dropped an unsupported object type on the outline View" );

    VLCMediaList * mediaList = [(VLCMedia *)[item representedObject] subitems];

    for (i = 0; i < [droppedItems count]; i++)
    {
        NSString * filename = [droppedItems objectAtIndex:i];
		VLCMedia *media = [VLCMedia mediaWithPath:filename];
        [mediaList lock];
		[mediaList insertMedia:media atIndex:index+1];
        [mediaList unlock];
    }
    return YES;
}

- (NSDragOperation)outlineView:(NSOutlineView *)outlineView validateDrop:(id < NSDraggingInfo >)info proposedItem:(id)item proposedChildIndex:(NSInteger)index
{
    NSArray *droppedItems = [[info draggingPasteboard] propertyListForType:@"VLCMediaURLType"];
    if( !droppedItems )
        droppedItems = [[info draggingPasteboard] propertyListForType:NSFilenamesPboardType];
    if( !droppedItems )
        droppedItems = [[info draggingPasteboard] propertyListForType:NSURLPboardType];

    if(! droppedItems ||
       ![item respondsToSelector:@selector(representedObject)] ||
       ![[item representedObject] isKindOfClass:[VLCMedia class]] )
    {
        return NSDragOperationNone;
    }

    return NSDragOperationMove;
}
@end

/******************************************************************************
 * VLCMainWindow
 */
@implementation VLCMainWindow
- (void)awakeFromNib;
{
    NSTableColumn * tableColumn;

    /* Check ib outlets */
    NSAssert( mainSplitView, @"No split view or wrong split view");
    NSAssert( fullScreenButton, @"No fullscreen button");

    /***********************************
     * Init the media player
     */
    mediaPlayer = [[VLCMediaPlayer alloc] initWithVideoView:videoView];

    /***********************************
     * CategoriesList OutlineView content
     */
    /* categoriesTreeController */ 
    categoriesTreeController = [[NSTreeController alloc] init];
    [categoriesTreeController setContent:controller.categories];
  
    [categoriesTreeController setChildrenKeyPath:@"childrenInCategoriesList"];
    //[categoriesTreeController bind:@"contentArray" toObject:controller withKeyPath:@"arrayOfMasters" options:nil];

    /* Bind the "name" table column */
    tableColumn = [categoriesListView tableColumnWithIdentifier:@"name"];
	[tableColumn bind:@"value" toObject: categoriesTreeController withKeyPath:@"arrangedObjects.descriptionInCategoriesList" options:nil];
    [tableColumn setEditable:YES];
    /* FIXME: this doesn't work obviously. */
	[tableColumn bind:@"editable" toObject: categoriesTreeController withKeyPath:@"arrangedObjects.editableInCategoriesList" options:nil];

    /* Use an ImageAndTextCell in the "name" table column */
    ImageAndTextCell * cell = [[ImageAndTextCell alloc] init];
    [cell setFont:[[tableColumn dataCell] font]];
    [cell setImageKeyPath:@"image"];

    [tableColumn setDataCell:cell];

    /* Other setup */
    [categoriesListView setSelectionHighlightStyle:NSTableViewSelectionHighlightStyleSourceList];
    [categoriesListView setDelegate:self];

    [categoriesListView registerForDraggedTypes:[NSArray arrayWithObjects:NSFilenamesPboardType, NSURLPboardType, @"VLCMediaURLType", nil]];
    [categoriesListView setDataSource: self];

    /***********************************
     * mediaListView setup
     */

    mediaArrayController = [[VLCMediaArrayController alloc] init];

    /* 1- Drag and drop */
    [mediaListView registerForDraggedTypes:[NSArray arrayWithObjects:NSFilenamesPboardType, NSURLPboardType, nil]];
    [mediaListView setDataSource:mediaArrayController];

    /* 2- Double click */
    [mediaListView setTarget:self];
    [mediaListView setDoubleAction:@selector(mediaListViewItemDoubleClicked:)];

	/* 3- binding for "title" column */
    tableColumn = [mediaListView tableColumnWithIdentifier:@"title"];
	[tableColumn bind:@"value" toObject: mediaArrayController withKeyPath:@"arrangedObjects.metaDictionary.title" options:nil];

	/* 4- binding for "state" column */
    tableColumn = [mediaListView tableColumnWithIdentifier:@"state"];
	[tableColumn bind:@"value" toObject: mediaArrayController withKeyPath:@"arrangedObjects.stateAsImage" options:nil];

    /* 5- Search & Predicate */
    NSMutableDictionary * bindingOptions = [NSMutableDictionary dictionary];
    [bindingOptions setObject:@"metaDictionary.title contains[c] $value" forKey:NSPredicateFormatBindingOption];
    [bindingOptions setObject:@"No Title" forKey:NSDisplayNameBindingOption];
    [mediaListSearchField bind:@"predicate" toObject: mediaArrayController withKeyPath:@"filterPredicate" options:bindingOptions];
    
    /* 6- Bind the @"contentArray" and contentMediaList of the mediaArrayController */
    [mediaArrayController bind:@"contentArray" toObject:categoriesTreeController withKeyPath:@"selection.childrenInCategoriesListForDetailView.media" options:nil];
    [mediaArrayController bind:@"contentMediaList" toObject:categoriesTreeController withKeyPath:@"selection.childrenInCategoriesListForDetailView.parentMediaList" options:nil];
    
    /* 7- Aspect */
    [mediaListView setSelectionHighlightStyle:NSTableViewSelectionHighlightStyleSourceList];
    [mediaListView setAllowsTypeSelect:YES];

    /***********************************
     * videoView setup
     */
    [videoView setItemsTree:controller.categories];
    [videoView setNodeKeyPath:@"childrenInVideoView"];
    [videoView setContentKeyPath:@"descriptionInVideoView"];
    [videoView setTarget:self];
    [videoView setAction:@selector(videoViewItemClicked:)];
    

    /***********************************
     * Other interface element setup
     */

    [mediaListItemsCount bind:@"displayPatternValue1" toObject:mediaArrayController withKeyPath:@"arrangedObjects.@count" options:[NSDictionary dictionaryWithObject:@"%{value1}@ items" forKey:NSDisplayPatternBindingOption]];
    [mediaListItemFetchedStatus bind:@"animate" toObject:categoriesTreeController withKeyPath:@"selection.currentlyFetchingItems" options:[NSDictionary dictionaryWithObject:@"%{value1}@ items" forKey:NSDisplayPatternBindingOption]];

    [fillScreenButton bind:@"value" toObject:videoView withKeyPath:@"fillScreen" options: [NSDictionary dictionaryWithObject:NSNegateBooleanTransformerName forKey:NSValueTransformerNameBindingOption]];
    [fullScreenButton bind:@"value" toObject:videoView withKeyPath:@"fullScreen" options: nil];
    [fullScreenButton bind:@"enabled" toObject:mediaPlayer withKeyPath:@"playing" options: nil];
    [fillScreenButton bind:@"enabled" toObject:mediaPlayer withKeyPath:@"playing" options: nil];

    [mediaReadingProgressSlider bind:@"enabled" toObject:mediaPlayer withKeyPath:@"media" options: [NSDictionary dictionaryWithObject:@"NonNilAsBoolTransformer" forKey:NSValueTransformerNameBindingOption]];
    [mediaReadingProgressSlider bind:@"enabled2" toObject:mediaPlayer withKeyPath:@"seekable" options: nil];

    [mediaReadingProgressSlider bind:@"value" toObject:mediaPlayer withKeyPath:@"position" options:
        [NSDictionary dictionaryWithObjectsAndKeys:@"Float10000FoldTransformer", NSValueTransformerNameBindingOption,
                                                  [NSNumber numberWithBool:NO], NSConditionallySetsEnabledBindingOption, nil ]];
    [mediaReadingProgressText bind:@"value" toObject:mediaPlayer withKeyPath:@"time.stringValue" options: nil];
    [mediaDescriptionText bind:@"value" toObject:mediaPlayer withKeyPath:@"description" options: nil];

    [navigatorViewToggleButton bind:@"value" toObject:self withKeyPath:@"navigatorViewVisible" options: nil];

    /* Playlist buttons */
    [removePlaylistButton bind:@"enabled" toObject:categoriesTreeController withKeyPath:@"selection.editableInCategoriesList" options: nil];
    [removePlaylistButton setTarget:categoriesTreeController];
    [removePlaylistButton setAction:@selector(remove:)];
    [addPlaylistButton setTarget:controller];
    [addPlaylistButton setAction:@selector(addPlaylist:)];

    [mainSplitView setDelegate:self];

    /* Last minute setup */
    [categoriesListView expandItem:nil expandChildren:YES];
    [categoriesListView selectRowIndexes:[NSIndexSet indexSetWithIndex:[categoriesListView numberOfRows] > 0 ? [categoriesListView numberOfRows]-1 : 0] byExtendingSelection:NO];
}

- (void)dealloc
{
    [navigatorView release];
    [mediaPlayer release];
    [categoriesTreeController release];
    [mediaArrayController release];
    [super dealloc];
}

- (void)mediaListViewItemDoubleClicked:(id)sender
{
    [mediaPlayer setMedia:[[mediaArrayController selectedObjects] objectAtIndex:0]];
    [mediaPlayer play];
}

- (void)videoViewItemClicked:(id)sender
{
    id object = [sender selectedObject];
    NSAssert( [object isKindOfClass:[VLCMedia class]], @"Object is not a VLCMedia" );

    [mediaPlayer setMedia:object];
    [mediaPlayer play];
}

- (BOOL)videoViewVisible
{
    NSAssert( mainSplitView && [[mainSplitView subviews] count] == 2, @"No split view or wrong split view");
    return  ([[[mainSplitView subviews] objectAtIndex:0] frame].size.height > 50.);
}

- (BOOL)navigatorViewVisible
{
    NSAssert( mainSplitView && [[mainSplitView subviews] count] == 2, @"No split view or wrong split view");
    return  ([[[mainSplitView subviews] objectAtIndex:1] frame].size.height > 6.);
}


- (void)setNavigatorViewVisible:(BOOL)visible
{
    NSAssert( mainSplitView && [[mainSplitView subviews] count] == 2, @"No split view or wrong split view");
    if(!([self navigatorViewVisible] ^ visible))
        return; /* Nothing to do */
    
    if(visible)
    {
        if( !navigatorHeight ) navigatorHeight = 100.f;
        if( ![self videoViewVisible] && ![self navigatorViewVisible] )
        {
            NSRect frame = [self frame];
            frame.origin.y -= navigatorHeight;
            frame.size.height += navigatorHeight;
            [[self animator] setFrame:frame display:YES];
        }
        else
            [[mainSplitView animator] setSliderPosition:([mainSplitView bounds].size.height - navigatorHeight - [mainSplitView dividerThickness])];
        /* Hack, because sliding cause some glitches */
        [navigatorView moveSubviewsToVisible];
    }
    else
    {
        navigatorHeight = [navigatorView bounds].size.height;
        NSRect frame0 = [self frame];
        NSRect frame1 = [[[mainSplitView subviews] objectAtIndex: 1] frame];
        frame0.size.height -= frame1.size.height;
        frame0.origin.y += frame1.size.height;
        frame1.size.height = 0;
        [[mainSplitView animator] setSliderPosition:([mainSplitView bounds].size.height)];
        /* Hack, because sliding cause some glitches */
        [navigatorView moveSubviewsToVisible];
    }
}
@end

@implementation VLCMainWindow (SplitViewDelegating)
- (CGFloat)splitView:(NSSplitView *)sender constrainSplitPosition:(CGFloat)proposedPosition ofSubviewAt:(NSInteger)offset
{
    CGFloat minHeight = 34.;

    /* Hack, because sliding cause some glitches */
    [navigatorView moveSubviewsToVisible];

    /* Get the bottom of the navigator view to get stuck at some points */
    if( [sender bounds].size.height - proposedPosition < minHeight*3./2. &&
        [sender bounds].size.height - proposedPosition >= minHeight/2 )
         return [sender bounds].size.height - minHeight;
    if( [sender bounds].size.height - proposedPosition < minHeight/2 )
         return [sender bounds].size.height;
    return proposedPosition;
}

- (void)splitView:(NSSplitView *)sender resizeSubviewsWithOldSize:(NSSize)oldSize
{
    [sender  adjustSubviews];

    /* Hack, because sliding cause some glitches */
    [navigatorView setFrame:[[navigatorView superview] bounds]];
    [navigatorView moveSubviewsToVisible];
}

- (void)splitViewWillResizeSubviews:(NSNotification *)aNotification
{
    /* Hack, because sliding cause some glitches */
    [navigatorView moveSubviewsToVisible];
    
    /* This could be changed from now on, so post a KVO notification */
    [self willChangeValueForKey:@"navigatorViewVisible"];
}
- (void)splitViewDidResizeSubviews:(NSNotification *)aNotification
{
    [self didChangeValueForKey:@"navigatorViewVisible"];
}
@end

@implementation VLCMainWindow (NSWindowDelegating)
- (NSSize)windowWillResize:(NSWindow *)window toSize:(NSSize)proposedFrameSize
{
    if( proposedFrameSize.height < 120.f)
        proposedFrameSize.height = [self minSize].height;
    return proposedFrameSize;
}
@end
