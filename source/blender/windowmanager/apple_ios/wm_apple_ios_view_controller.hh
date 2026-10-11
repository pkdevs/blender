/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Private to the native iPad and Apple Vision Pro interface: the view controller of Blender's
 * window and the helpers its files share, `wm_apple_ios_view_controller*.mm` and
 * `wm_apple_ios_interface.mm`.
 */

#pragma once

#import <MetalKit/MetalKit.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include "BLI_utildefines.h"

#include "GHOST_IOSNativeInterface.hh"
#include "GHOST_IOSRuntime.hh"

#import "wm_apple_ios_assets.hh"
#import "wm_apple_ios_blend_document.hh"
#import "wm_apple_ios_compile_indicator.hh"
#import "wm_apple_ios_file_drop.hh"
#import "wm_apple_ios_icons.hh"
#import "wm_apple_ios_launcher.hh"
#import "wm_apple_ios_platform.hh"
#import "wm_apple_ios_redo_strip.hh"
#import "wm_apple_ios_render.hh"
#import "wm_apple_ios_scene_panels.hh"
#import "wm_apple_ios_timeline.hh"

extern "C" void WM_apple_ios_ui_command_enqueue(int command);
extern "C" void WM_apple_ios_ui_operator_enqueue(const char *op_name);
extern "C" void WM_apple_ios_ui_operator_at_position_enqueue(const char *op_name, int x, int y);
extern "C" void WM_apple_ios_ui_operator_enum_enqueue(const char *op_name,
                                                      const char *prop_name,
                                                      const char *prop_value);
extern "C" uint64_t WM_apple_ios_ui_menu_request(const char *route, bool execute);
extern "C" uint64_t WM_apple_ios_ui_menu_at_position_request(const char *route,
                                                             bool execute,
                                                             int x,
                                                             int y);
extern "C" char *WM_apple_ios_ui_menu_take_result(uint64_t identifier);
extern "C" void WM_apple_ios_ui_setting_enqueue(const char *setting, const char *value);
extern "C" void WM_apple_ios_ui_tool_set_by_id(const char *tool_id);
extern "C" void WM_apple_ios_ui_brush_activate(const char *brush_id);
extern "C" void WM_apple_ios_ui_view_roll_enqueue(float angle);
extern "C" void WM_apple_ios_ui_navigation_hover_set(bool active);
extern "C" void WM_apple_ios_ui_pencil_hover_set(bool active, int x, int y);
extern "C" char *WM_apple_ios_ui_toolbar_copy_json();
extern "C" char *WM_apple_ios_ui_toolbar_take_json();
extern "C" char *WM_apple_ios_ui_panels_take_json();
extern "C" char *WM_apple_ios_ui_timeline_take_json();
extern "C" void WM_apple_ios_ui_bottom_tray_set(float fraction);
extern "C" char *WM_apple_ios_ui_assets_take_json();
extern "C" void WM_apple_ios_ui_toolbar_free_json(char *json);
enum WMIOSUICommand {
  WM_IOS_UI_COMMAND_SET_MODE_OBJECT = 0,
  WM_IOS_UI_COMMAND_SET_MODE_EDIT = 1,
  WM_IOS_UI_COMMAND_SET_MODE_SCULPT = 2,
  WM_IOS_UI_COMMAND_SET_MODE_VERTEX_PAINT = 3,
  WM_IOS_UI_COMMAND_SET_MODE_WEIGHT_PAINT = 4,
  WM_IOS_UI_COMMAND_SET_MODE_TEXTURE_PAINT = 5,
  WM_IOS_UI_COMMAND_SELECT_TOOL_SELECT_BOX = 6,
  WM_IOS_UI_COMMAND_SELECT_TOOL_CURSOR = 7,
  WM_IOS_UI_COMMAND_SELECT_TOOL_MOVE = 8,
  WM_IOS_UI_COMMAND_SELECT_TOOL_ROTATE = 9,
  WM_IOS_UI_COMMAND_SELECT_TOOL_SCALE = 10,
  WM_IOS_UI_COMMAND_SELECT_TOOL_TRANSFORM = 11,
  WM_IOS_UI_COMMAND_SELECT_TOOL_ANNOTATE = 12,
  WM_IOS_UI_COMMAND_SELECT_TOOL_MEASURE = 13,
  WM_IOS_UI_COMMAND_FRAME_SELECTED = 14,
  WM_IOS_UI_COMMAND_RESET_VIEW = 15,
  WM_IOS_UI_COMMAND_ORBIT_LEFT = 16,
  WM_IOS_UI_COMMAND_ORBIT_RIGHT = 17,
  WM_IOS_UI_COMMAND_ORBIT_UP = 18,
  WM_IOS_UI_COMMAND_ORBIT_DOWN = 19,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_WIREFRAME = 20,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_SOLID = 21,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_MATERIAL = 22,
  WM_IOS_UI_COMMAND_SET_VIEWPORT_RENDERED = 23,
  WM_IOS_UI_COMMAND_VIEW_ALL = 24,
  WM_IOS_UI_COMMAND_VIEW_LOCAL = 25,
  WM_IOS_UI_COMMAND_VIEW_CAMERA = 26,
  WM_IOS_UI_COMMAND_VIEW_PERSP_ORTHO = 27,
  WM_IOS_UI_COMMAND_VIEW_FRONT = 28,
  WM_IOS_UI_COMMAND_VIEW_BACK = 29,
  WM_IOS_UI_COMMAND_VIEW_RIGHT = 30,
  WM_IOS_UI_COMMAND_VIEW_LEFT = 31,
  WM_IOS_UI_COMMAND_VIEW_TOP = 32,
  WM_IOS_UI_COMMAND_VIEW_BOTTOM = 33,
};

/* Buttons drawn by SwiftUI that stay UIKit controls (`intern/swift/GlassButton.swift`), for the
 * controls this view controller lays out, animates and hit-tests itself. */
@interface UIButton (WMIOSSwiftButton)
@property(nonatomic, copy) NSString *buttonTitle;
@property(nonatomic, retain) UIImage *buttonImage;
@property(nonatomic, copy) NSString *buttonStyle;
@property(nonatomic, retain) UIColor *selectedTint;
@property(nonatomic, retain) UIColor *foreground;
@property(nonatomic, retain) UIColor *selectedForeground;
@property(nonatomic, retain) UIFont *buttonFont;
@property(nonatomic) CGFloat imagePadding;
@property(nonatomic) NSDirectionalEdgeInsets buttonInsets;
@property(nonatomic) CGFloat buttonCornerRadius;
@property(nonatomic) BOOL imageTrailing;
@property(nonatomic) BOOL contentLeading;
@end

@interface WMIOSPassThroughOverlayView : UIView
@end
@interface WMIOSPassThroughStackView : UIStackView
@end
@interface WMIOSToolStackView : WMIOSPassThroughStackView
{
  UIButton *_selection;
  UIButton *_selectionTarget;
}
@end
@interface WMIOSNavigationBar : UINavigationBar
@end
@interface WMIOSNavigationController : UINavigationController
@end
@interface WMIOSPassThroughScrollView : UIScrollView
@end
@interface WMIOSButtonScrollView : UIScrollView
@end
@interface WMIOSFadingButtonScrollView : WMIOSButtonScrollView
@end
@interface WMIOSPassThroughVisualEffectView : UIVisualEffectView
@end

/* Helpers shared by the interface's files, see `GHOST_WindowIOS.mm`. */
UIImage *blenderIOSIcon(NSString *key, NSString *systemImage);
UIImage *blenderIOSBrushImage(NSString *previewPath, CGFloat diameter);
UIImage *blenderIOSCommandIcon(NSInteger command, NSString *systemImage);
UIImage *blenderIOSActionIcon(NSString *opName, NSString *propertyName, NSString *propertyValue);
Class blenderIOSSwiftClass(NSString *name);
UIButton *blenderIOSSwiftButton();
NSString *blenderIOSMenuRoute(NSDictionary *route);
UIImage *blenderIOSMenuIcon(NSDictionary *item);
NSArray *blenderIOSMenuResult(uint64_t identifier);
UIMenu *blenderIOSNativeSubmenu(NSDictionary *item, NSString *identifier);
NSArray<UIMenuElement *> *blenderIOSNativeMenuElements(NSArray *items);

/** The SwiftUI Settings sheet, `WMIOSSwiftSettings` in `SettingsSheet.swift`. */
@protocol WMIOSSettings <NSObject>
+ (BOOL)fingerUsesTool;
+ (BOOL)showQuickActions;
- (instancetype)initWithPresenter:(UIViewController *)presenter;
- (void)present;
- (void)updatePreferences:(id)preferences;
@property(nonatomic, copy) void (^fingerUsesToolChanged)(BOOL);
@property(nonatomic, copy) void (^showQuickActionsChanged)(BOOL);
@end
#if defined(APPLE_TARGET_VISIONOS)
/** `VisionChrome.swift`: the window's controls as visionOS ornaments. */
@protocol WMIOSVisionChrome <NSObject>
- (instancetype)initWithController:(UIViewController *)controller
                            choose:(void (^)(NSString *))choose;
- (void)updateTools:(NSArray *)tools
             images:(NSDictionary<NSString *, UIImage *> *)images
     selectedImages:(NSDictionary<NSString *, UIImage *> *)selectedImages
              menus:(NSDictionary<NSString *, UIMenu *> *)menus;
- (void)setActiveTool:(NSString *)identifier;
- (void)setContextMenu:(UIMenu *)menu;
- (void)setActions:(NSArray<UIView *> *)actions;
@end
#endif
/** The SwiftUI document window, `WMIOSSwiftDocumentController` in `DocumentWindow.swift`. */
@protocol WMIOSDocumentController <GHOST_IOSWindowController>
- (instancetype)initWithMetalView:(MTKView *)metalView window:(void *)window;
- (void)updateDocumentControls;
@end

@interface WMIOSViewController
    : UIViewController <GHOST_IOSWindowController, UIGestureRecognizerDelegate>

- (instancetype)initWithMetalKitView:(MTKView *)mtkView window:(GHOST_WindowIOS *)window;
- (void)detachBlender;
- (void)updateDocumentControls;

@end

@interface WMIOSViewController ()
{
  MTKView *_view;
  GHOST_WindowIOS *_ghost_window;
  UIActivityIndicatorView *_viewport_loading_indicator;
  UIEditMenuInteraction *_actions_edit_menu_interaction;
  WMIOSPassThroughOverlayView *_overlay_view;
  WMIOSNavigationBar *_top_bar;
  UINavigationItem *_top_bar_item;
  NSLayoutConstraint *_top_bar_height_constraint;
  UIBarButtonItem *_mode_button;
  /* Blender's mode list for the active object, and its current mode identifier. */
  NSArray *_mode_items;
  NSString *_object_mode;
  UIBarButtonItem *_actions_button;
  UIBarButtonItem *_share_button;
  NSObject<WMIOSRender> *_render;
  NSObject<WMIOSSettings> *_settings;
  /* The iPad's saved Pencil and touch choice is applied once the view has a window. */
  BOOL _touch_settings_applied;
  NSArray *_quick_actions;
  NSDictionary *_viewport_state;
  /* Back to the document launcher, through the system's back action. */
  UIBarButtonItem *_documents_button;
  UIBarButtonItem *_viewport_shading_button;
  UIBarButtonItem *_undo_button;
  UIBarButtonItem *_finger_input_button;
  NSObject<WMIOSScenePanels> *_scene_panels;
  NSObject<WMIOSRedoStrip> *_redo_strip;
  NSObject<WMIOSTimeline> *_timeline;
  NSObject<WMIOSAssets> *_assets;
  WMIOSFileDrop *_file_drop;
  WMIOSCompileIndicator *_compile_indicator;
  BOOL _finger_tool_enabled;
  UIView<GHOSTIOSPencilMenu> *_pencil_menu;
  UIBarButtonItemGroup *_mesh_select_group;
  UISegmentedControl *_mesh_select_control;
  NSMutableArray *_mesh_select_buttons;
  NSMutableArray *_toolbar_buttons;
  NSMutableArray *_action_buttons;
  UIBarButtonItemGroup *_top_settings_group;
  NSDictionary *_top_settings_values;
  WMIOSPassThroughVisualEffectView *_tool_rail;
#if defined(APPLE_TARGET_VISIONOS)
  NSObject<WMIOSVisionChrome> *_vision_chrome;
#endif
  UIVisualEffect *_tool_rail_glass_effect;
  UIScrollView *_tool_scroll;
  UIButton *_tool_drag_handle;
  NSLayoutConstraint *_tool_drag_handle_height_constraint;
  UIStackView *_tool_stack;
  WMIOSPassThroughVisualEffectView *_action_rail;
  UIVisualEffect *_action_rail_glass_effect;
  UIScrollView *_action_scroll;
  UIStackView *_action_stack;
  UIView *_action_group;
  UIButton *_action_toggle;
  NSLayoutConstraint *_action_expanded_width_constraint;
  NSLayoutConstraint *_action_collapsed_width_constraint;
  NSLayoutConstraint *_action_toggle_width_constraint;
  NSLayoutConstraint *_action_toggle_trailing_constraint;
  NSLayoutConstraint *_action_group_leading_constraint;
  NSLayoutConstraint *_action_group_center_constraint;
  CGRect _redo_lane_rect;
  BOOL _redo_lane_wide;
  NSLayoutConstraint *_action_scroll_trailing_constraint;
  /* Blender's full context menu, kept at the end of the expanded quick actions. */
  UIButton *_context_menu_button;
  NSLayoutConstraint *_context_menu_width_constraint;
  NSLayoutConstraint *_context_menu_spacing_constraint;
  BOOL _context_menu_available;
  NSLayoutConstraint *_redo_pill_width_constraint;
  NSLayoutConstraint *_redo_pill_min_width_constraint;
  BOOL _actions_collapsed;
  NSUInteger _action_transition_generation;
  UIView *_view_gizmo_hotspot;
  UIView *_view_gizmo_feedback;
  NSArray *_view_gizmo_geometry;
#if !defined(APPLE_TARGET_VISIONOS)
  UISelectionFeedbackGenerator *_view_gizmo_selection_feedback;
  UIImpactFeedbackGenerator *_view_gizmo_open_feedback;
#endif
  BOOL _view_gizmo_hovered;
  BOOL _view_gizmo_pressed;
  NSMutableArray *_view_pie_buttons;
  NSTimer *_toolbar_refresh_timer;
  NSTimer *_panels_refresh_timer;
  NSArray *_native_menubar_items;
  uint64_t _native_menubar_request;
  NSString *_toolbar_signature;
  NSString *_action_signature;
  NSString *_active_tool_identifier;
  NSLayoutConstraint *_tool_rail_width_constraint;
  NSLayoutConstraint *_tool_rail_height_constraint;
  NSLayoutConstraint *_tool_rail_bottom_constraint;
  NSLayoutConstraint *_tool_rail_center_y_constraint;
  NSLayoutConstraint *_action_phone_bottom_constraint;
  /* The open asset pane is a tray along the bottom; these controls sit on top of it. */
  NSLayoutConstraint *_action_bottom_constraint;
  CGFloat _bottom_tray;
  NSLayoutConstraint *_action_phone_trailing_constraint;
  UIButton *_toolbar_hover_button;
  UIVisualEffectView *_toolbar_label_bubble;
  UILabel *_toolbar_label;
  UIButton *_toolbar_label_button;
  NSUInteger _toolbar_label_generation;
  BOOL _toolbar_label_transient;
  UIButton *_view_pie_hover_button;
  BOOL _tool_rail_docked_left_center;
  /* Brush rail: a second left rail listing the brush assets of the current paint mode. */
  WMIOSPassThroughVisualEffectView *_brush_rail;
  UIScrollView *_brush_scroll;
  UIStackView *_brush_stack;
  UIButton *_brush_drag_handle;
  NSMutableArray *_brush_buttons;
  NSString *_brush_signature;
  NSString *_active_brush_identifier;
  NSLayoutConstraint *_brush_rail_leading_constraint;
  NSLayoutConstraint *_brush_rail_width_constraint;
  NSLayoutConstraint *_brush_rail_height_constraint;
  NSLayoutConstraint *_brush_rail_bottom_constraint;
  NSLayoutConstraint *_brush_drag_handle_height_constraint;
  UIButton *_brush_hover_button;
  BOOL _brush_rail_available;
  BOOL _brush_rail_docked_left_center;
  BOOL _brush_rail_expanded;
  BOOL _brush_press_select_active;
  BOOL _toolbar_expanded;
  /* Blender's own interface, as on a desktop computer, with the native controls hidden. */
  BOOL _desktop_interface;
  /* Full screen: the native interface is hidden except for the button that brings it back. */
  BOOL _interface_hidden;
  UIButton *_show_interface_button;
  BOOL _toolbar_press_select_active;
  BOOL _view_pie_visible;
}

- (instancetype)initWithMetalKitView:(MTKView *)mtkView window:(GHOST_WindowIOS *)window;
- (void)loadView;
- (void)viewDidLoad;
- (void)installDocumentControls;
- (void)updateDocumentControls;
- (void)showDocuments:(id)sender;
- (void)updateMTKViewDrawableScale;
- (void)installViewportLoadingIndicator;
- (void)updateViewportLoadingIndicator;
- (void)detachBlender;
- (void)dealloc;
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
       shouldReceiveTouch:(UITouch *)touch;
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gestureRecognizer;
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:
        (UIGestureRecognizer *)otherGestureRecognizer;
- (void)refreshScenePanelsFromBlender;
- (void)applyTemplatePresentation:(NSNotification *)notification;
- (void)newDocument:(id)sender;
- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder;
- (void)setOverlayShown:(BOOL)shown;
- (void)setInterfaceHidden:(BOOL)hidden;
- (void)hoverShowInterface:(UIHoverGestureRecognizer *)hover;
- (BOOL)prefersStatusBarHidden;
- (void)setDesktopInterface:(BOOL)desktop;
- (void)blenderSnapshotsPublished:(NSNotification *)notification;
- (void)refreshToolbarFromBlender;
- (void)applyToolbarModel:(NSDictionary *)model;
- (void)viewDidLayoutSubviews;
- (void)installIPadOverlay;
- (void)handleGesture:(UIGestureRecognizer *)gestureRecognizer;
- (BOOL)prefersHomeIndicatorAutoHidden;

@end

/** See `wm_apple_ios_view_controller_tools.mm`. */
@interface WMIOSViewController (Tools)

- (UIButton *)overlayButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                             command:(NSInteger)command
                     clearGlassStyle:(BOOL)clearGlassStyle;
- (UIButton *)overlayButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                             command:(NSInteger)command;
- (UIButton *)toolbarButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                      toolIdentifier:(NSString *)toolIdentifier
                                mode:(NSString *)mode;
- (UIImage *)toolbarImageForTool:(NSDictionary *)tool mode:(NSString *)mode;
- (UIImage *)toolbarImageForTool:(NSDictionary *)tool
                            mode:(NSString *)mode
                        selected:(BOOL)selected;
- (void)configureToolbarButton:(UIButton *)button tool:(NSDictionary *)tool mode:(NSString *)mode;
- (UIMenu *)toolbarMenuForTool:(NSDictionary *)tool mode:(NSString *)mode;
- (void)updateToolbarMenuAvailabilityForButton:(UIButton *)button;
- (BOOL)isToolbarListVisible;
- (void)setActiveToolbarToolIdentifier:(NSString *)toolIdentifier;
#if defined(APPLE_TARGET_VISIONOS)
- (void)updateVisionToolPalette:(NSArray *)tools mode:(NSString *)mode;
#endif
- (CGFloat)expandedToolbarHeight;
- (void)updateToolbarHeightForCurrentState;
- (CGFloat)expandedToolbarWidth;
- (CGFloat)toolRailBottomDockCenterY;
- (CGFloat)toolRailCenterDockCenterY;
- (void)setToolRailDockedLeftCenter:(BOOL)dockedLeftCenter animated:(BOOL)animated;
- (void)handleToolbarDockPan:(UIPanGestureRecognizer *)gesture;
- (UIButton *)toolbarDragHandle;
- (void)collapseToolbarFromHandle:(id)sender;
- (void)handleToolbarHandlePan:(UIPanGestureRecognizer *)gesture;
- (void)setToolbarGeometryExpanded:(BOOL)expanded;
- (void)updateToolbarVisibilityAnimated:(BOOL)animated;
- (void)setToolbarExpanded:(BOOL)expanded animated:(BOOL)animated;
- (NSDictionary *)toolbarModel;
- (NSString *)toolbarSignatureForTools:(NSArray *)tools mode:(NSString *)mode;
- (void)rebuildToolbarWithTools:(NSArray *)tools mode:(NSString *)mode;
- (void)handleToolbarToolButton:(UIButton *)sender;
- (void)layoutToolbarLabel;
- (void)hideToolbarLabelAnimated:(BOOL)animated;
- (void)showToolbarLabelForButton:(UIButton *)button transient:(BOOL)transient;
- (void)handleToolbarHover:(UIHoverGestureRecognizer *)gesture;
- (UIButton *)toolbarButtonAtGestureLocation:(UIGestureRecognizer *)gesture;
- (void)setToolbarHoverButton:(UIButton *)hoverButton;
- (void)finishToolbarPressSelectWithButton:(UIButton *)button;
- (void)handleToolbarPressSelectGesture:(UILongPressGestureRecognizer *)gesture;

@end

/** See `wm_apple_ios_view_controller_brushes.mm`. */
@interface WMIOSViewController (Brushes)

- (void)installBrushRail;
- (void)installBrushRailLayout;
- (BOOL)isBrushListVisible;
- (void)updateBrushRailGeometry;
- (void)updateBrushRailWithModel:(id)model;
- (void)setBrushRailAvailable:(BOOL)available;
- (UIButton *)brushButtonForItem:(NSDictionary *)item;
- (void)rebuildBrushRailWithItems:(NSArray *)items;
- (void)setActiveBrushIdentifier:(NSString *)brushIdentifier;
- (void)updateBrushRailVisibilityAnimated:(BOOL)animated;
- (void)setBrushRailExpanded:(BOOL)expanded animated:(BOOL)animated;
- (void)setBrushRailDockedLeftCenter:(BOOL)dockedLeftCenter animated:(BOOL)animated;
- (void)handleBrushDockPan:(UIPanGestureRecognizer *)gesture;
- (void)collapseBrushRailFromHandle:(id)sender;
- (void)handleBrushHandlePan:(UIPanGestureRecognizer *)gesture;
- (void)activateBrushButton:(UIButton *)button;
- (void)handleBrushButton:(UIButton *)sender;
- (UIButton *)brushButtonAtGestureLocation:(UIGestureRecognizer *)gesture;
- (void)setBrushHoverButton:(UIButton *)hoverButton;
- (void)handleBrushPressSelectGesture:(UILongPressGestureRecognizer *)gesture;
- (void)handleBrushHover:(UIHoverGestureRecognizer *)gesture;

@end

/** See `wm_apple_ios_view_controller_actions.mm`. */
@interface WMIOSViewController (Actions) <UIEditMenuInteractionDelegate,
                                          GHOSTIOSPencilMenuDelegate>

- (NSArray<NSDictionary *> *)radialItemsFromBlenderMenu:(NSArray *)sources;
- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
    requestItemsForRoute:(NSDictionary *)route
                 atPoint:(CGPoint)point
              completion:(void (^)(NSArray<NSDictionary *> *))completion;
- (void)pencilMenu:(UIView<GHOSTIOSPencilMenu> *)menu
     didChooseItem:(NSDictionary *)item
           atPoint:(CGPoint)point;
- (UIMenu *)nativeBlenderContextMenu;
- (UIMenu *)editMenuInteraction:(UIEditMenuInteraction *)interaction
           menuForConfiguration:(UIEditMenuConfiguration *)configuration
               suggestedActions:(NSArray<UIMenuElement *> *)suggestedActions;
- (CGRect)editMenuInteraction:(UIEditMenuInteraction *)interaction
    targetRectForConfiguration:(UIEditMenuConfiguration *)configuration;
- (void)handleActionsMenuPress:(UILongPressGestureRecognizer *)gesture;
- (void)updateAxisConstraintButton:(UIButton *)button selected:(BOOL)selected;
- (void)updateAxisConstraintsWithActions:(NSArray *)actions;
- (UIButton *)actionButtonWithTitle:(NSString *)title
                        systemImage:(NSString *)systemImage
                        operator
                                   :(NSString *)opName
                           property:(NSString *)propertyName
                              value:(NSString *)propertyValue
                                key:(NSString *)keyName;
- (UIButton *)contextMenuButton;
- (void)updateContextMenuButton;
- (NSString *)actionSignatureForActions:(NSArray *)actions;
- (void)rebuildActionStripWithActions:(NSArray *)actions;
- (BOOL)actionRailEmpty:(CGFloat)redo_pill_width;
- (void)installActionStripToggle;
- (void)layoutRedoStrip;
- (void)updateRedoPillLayout:(BOOL)animated;
- (void)toggleActionStrip:(UIButton *)sender;
- (void)setActionsCollapsed:(BOOL)collapsed animated:(BOOL)animated;
- (void)updatePhoneActionStripLayout;
- (void)installActionStripLayout;
- (void)handleActionButton:(UIButton *)sender;
- (GHOST_TKey)ghostKeyForName:(NSString *)keyName;
- (void)sendBlenderKeyNamed:(NSString *)keyName;

@end

/** See `wm_apple_ios_view_controller_view_gizmo.mm`. */
@interface WMIOSViewController (ViewGizmo)

- (UIButton *)viewPieButtonWithTitle:(NSString *)title
                         systemImage:(NSString *)systemImage
                             command:(NSInteger)command;
- (void)setViewPieHoverButton:(UIButton *)hoverButton;
- (void)updateViewGizmoFeedback;
- (void)handleViewGizmoHover:(UIHoverGestureRecognizer *)gesture;
- (void)handleViewGizmoPress:(UIControl *)control;
- (void)handleViewGizmoRelease:(UIControl *)control;
- (void)cancelViewGizmoInteraction;
- (void)layoutViewPie;
- (UIButton *)viewPieButtonAtGestureLocation:(UIGestureRecognizer *)gesture;
- (void)setViewPieVisible:(BOOL)visible animated:(BOOL)animated;
- (void)handleViewGizmoDrag:(UIPanGestureRecognizer *)gesture;
- (void)handleViewGizmoHold:(UILongPressGestureRecognizer *)gesture;
- (void)handleViewGizmoTap:(UITapGestureRecognizer *)gesture;
- (void)handleOverlayCommand:(UIButton *)sender;

@end

/** See `wm_apple_ios_view_controller_top_bar.mm`. */
@interface WMIOSViewController (TopBar)

- (UIBarButtonItem *)documentsButton;
- (void)refreshNativeMenuBar;
- (void)setFingerInputMode:(BOOL)useTool;
- (UIBarButtonItem *)fingerInputButton;
- (UIAction *)modeActionWithTitle:(NSString *)title command:(NSInteger)command;
- (void)setModePickerModes:(NSArray *)modes current:(NSString *)current;
- (NSString *)modeTitleForIdentifier:(NSString *)mode;
- (void)setModePickerTitleForMode:(NSString *)mode;
- (UIAction *)viewportShadingActionWithTitle:(NSString *)title
                                 systemImage:(NSString *)systemImage
                                     command:(NSInteger)command;
- (void)setViewportShadingValue:(NSString *)value;
- (UIAction *)settingsActionWithTitle:(NSString *)title
                          systemImage:(NSString *)systemImage
                              setting:(NSString *)setting
                                value:(NSString *)value;
- (UIBarButtonItem *)settingsMenuButtonWithAccessibilityLabel:(NSString *)accessibilityLabel
                                                  systemImage:(NSString *)systemImage
                                                        title:(NSString *)title
                                                     children:(NSArray *)children;
- (UIBarButtonItemGroup *)topSettingsControlGroup;
- (void)setTopSettingsValues:(NSDictionary *)values;
- (BOOL)isTransformToolIdentifier:(NSString *)toolIdentifier;
- (void)setTopSettingsVisible:(BOOL)visible animated:(BOOL)animated;
- (UIBarButtonItem *)meshSelectModeButtonWithTitle:(NSString *)title
                                       systemImage:(NSString *)systemImage
                                             value:(NSString *)value;
- (UIBarButtonItemGroup *)meshSelectModeControlGroup;
- (void)setMeshSelectModeVisible:(BOOL)visible animated:(BOOL)animated;
- (void)setMeshSelectModeValue:(NSString *)value;
- (void)showViewportShading:(UIBarButtonItem *)button;
- (UIBarButtonItem *)viewportShadingButton;
- (void)setUndoAvailable:(id)value;
- (void)handleUndoButton:(UIBarButtonItem *)button;
- (UIBarButtonItem *)undoButton;
- (UIBarButtonItem *)modePickerButton;
- (void)updateActionsButtonMenu;
- (void)saveDocumentAs;
- (void)shareDocument;
- (void)saveAsTemplate;
- (UIBarButtonItem *)actionsButton;
- (void)installNativeTopBar;

@end
