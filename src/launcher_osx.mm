// ztpp — src/launcher_osx.mm: НАТИВНОЕ окно выбора ROM на macOS (Cocoa), по образцу
// стартап-окна eduke32 (source/duke3d/src/startosx.game.mm). В отличие от SDL-фолбэка
// (launcher.hpp runSdl — самодельный фреймбуфер + bitmap-шрифт), здесь используются
// РЕАЛЬНЫЕ виджеты ОС: NSWindow + NSTableView (список ROM) + NSButton (Play/Quit/Browse) +
// NSOpenPanel (нативный файловый диалог) + drag&drop.
//
// Data-слой (scanRoms/probeRom/makeState/saveLastRom из launcher.hpp) — ОБЩИЙ с SDL-фолбэком;
// тут только UI. Окно показывается ДО инициализации SDL-окна игры, на общем
// [NSApplication sharedApplication] — SDL позже подхватывает тот же singleton (как в eduke32:
// osx_preopen → startwin_run → app_main → SDL). Модальность — ручной событийный цикл
// nextEventMatchingMask: с сентинелом result (не runModalForWindow:), чтобы не завязываться на
// NIB и работать до finishLaunching. Сборка: CMake добавляет файл только на APPLE (framework
// Cocoa, ARC). На прочих ОС launcher::run() уходит в runSdl.

// AssertMacros.h исторически определял check()/verify() как макросы — гасим на всякий случай,
// чтобы не задеть идентификаторы в C++ заголовках проекта.
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#import <Cocoa/Cocoa.h>

#include "launcher_data.hpp"   // launcher::Entry / scanRoms / makeState / probeRom / buildLabel / saveLastRom
#include "version.hpp"         // ztppVersion() — версия в заголовке лаунчера
#include "logo_data.hpp"       // встроенный логотип (Dock-иконка)

// ⭐Dock-иконка macOS из встроенного PNG-логотипа (зовёт и main через app_icon.hpp)
void ztppSetDockIconMac(const void* png, unsigned len) {
    @autoreleasepool {
        NSData* d = [NSData dataWithBytes:png length:len];
        NSImage* img = [[NSImage alloc] initWithData:d];
        if (img) [NSApp setApplicationIconImage:img];
    }
}
                               // (НЕ launcher.hpp — тот тянет ui.hpp с типом Rect, конфликт с Cocoa/MacTypes.h)
#include <string>
#include <vector>

// Состояние модального цикла (как retval в eduke32 startwin_run).
namespace { enum { LR_RUNNING = -1, LR_QUIT = 0, LR_PLAY = 1 }; }

// ─── Контроллер окна: источник данных таблицы + делегат окна ────────────────────
@interface ZtppLauncherCtl : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate>
@property (assign) int result;
- (instancetype)initWithEntries:(std::vector<launcher::Entry>*)entries selected:(int)sel;
- (NSWindow*)window;
- (std::string)selectedPathStd;
- (void)addRomAtPath:(NSString*)path;
- (void)cancel:(id)sender;      // ⌘Q из меню
@end

// ─── contentView, принимающий drag&drop файлов (форвардит в контроллер) ─────────
@interface ZtppDropView : NSView
@property (weak) ZtppLauncherCtl* ctl;
@end

@implementation ZtppDropView
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender { return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray* urls = [[sender draggingPasteboard] readObjectsForClasses:@[[NSURL class]] options:nil];
    if ([urls count] == 0) return NO;
    [_ctl addRomAtPath:[[urls firstObject] path]];
    return YES;
}
@end

@implementation ZtppLauncherCtl {
    std::vector<launcher::Entry>* _entries;   // владелец — стек runNative (живёт весь модальный цикл)
    NSWindow*    _win;
    NSTableView* _table;
    NSTextField* _detail1;
    NSTextField* _detail2;
    NSButton*    _playBtn;
    std::string  _selectedPath;
}

- (instancetype)initWithEntries:(std::vector<launcher::Entry>*)entries selected:(int)sel {
    if ((self = [super init])) {
        _entries = entries;
        _result  = LR_RUNNING;
        [self buildWindow];
        if (sel >= 0 && sel < (int)_entries->size()) {
            [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:sel] byExtendingSelection:NO];
            [_table scrollRowToVisible:sel];
        }
        [self refreshDetail];
    }
    return self;
}

- (NSWindow*)window { return _win; }
- (std::string)selectedPathStd { return _selectedPath; }

// ─── Фабрики виджетов (мелкий системный шрифт, как у eduke32 makeLabel/makeComboBox) ───
- (NSTextField*)makeLabel:(NSRect)f font:(NSFont*)font color:(NSColor*)color {
    NSTextField* t = [[NSTextField alloc] initWithFrame:f];
    [t setEditable:NO]; [t setSelectable:NO]; [t setBordered:NO]; [t setBezeled:NO];
    [t setDrawsBackground:NO]; [t setFont:font]; [t setTextColor:color];
    return t;
}
- (NSButton*)makeButton:(NSString*)title frame:(NSRect)f action:(SEL)sel {
    NSButton* b = [[NSButton alloc] initWithFrame:f];
    [b setTitle:title]; [b setBezelStyle:NSBezelStyleRounded];
    [b setTarget:self]; [b setAction:sel];
    return b;
}
- (void)addColumn:(NSString*)ident title:(NSString*)title width:(CGFloat)w align:(NSTextAlignment)al {
    NSTableColumn* c = [[NSTableColumn alloc] initWithIdentifier:ident];
    [[c headerCell] setStringValue:title];
    [c setWidth:w]; [c setEditable:NO];
    [[c dataCell] setAlignment:al];
    [_table addTableColumn:c];
}

- (void)buildWindow {
    ztppSetDockIconMac(ZTPP_LOGO_PNG, ZTPP_LOGO_PNG_LEN);   // ⭐Dock-иконка сразу в лаунчере
    const NSRect frame = NSMakeRect(0, 0, 640, 440);
    const CGFloat W = frame.size.width, H = frame.size.height;

    _win = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    [_win setTitle:[NSString stringWithFormat:@"ZTPP v%s — Select a ROM", ztppVersion()]];
    [_win setReleasedWhenClosed:NO];
    [_win setDelegate:self];
    [_win setContentMinSize:NSMakeSize(520, 360)];

    ZtppDropView* content = [[ZtppDropView alloc] initWithFrame:frame];
    content.ctl = self;
    [content registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    [_win setContentView:content];

    // Заголовок
    NSTextField* title = [self makeLabel:NSMakeRect(16, H - 38, W - 32, 24)
                                    font:[NSFont boldSystemFontOfSize:16] color:[NSColor labelColor]];
    [title setStringValue:[NSString stringWithFormat:@"Zero Tolerance — CPP Port  v%s", ztppVersion()]];
    [title setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [content addSubview:title];

    NSTextField* sub = [self makeLabel:NSMakeRect(16, H - 58, W - 32, 16)
                                  font:[NSFont systemFontOfSize:11] color:[NSColor secondaryLabelColor]];
    [sub setStringValue:@"Select a ROM to launch  ·  drag & drop or Browse to add one"];
    [sub setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
    [content addSubview:sub];

    // Список ROM
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(16, 96, W - 32, H - 96 - 70)];
    [scroll setHasVerticalScroller:YES];
    [scroll setBorderType:NSBezelBorder];
    [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    _table = [[NSTableView alloc] initWithFrame:[[scroll contentView] bounds]];
    [_table setUsesAlternatingRowBackgroundColors:YES];
    [_table setAllowsMultipleSelection:NO];
    [_table setAllowsEmptySelection:YES];
    [_table setColumnAutoresizingStyle:NSTableViewLastColumnOnlyAutoresizingStyle];
    [self addColumn:@"file"   title:@"File"   width:250 align:NSTextAlignmentLeft];
    [self addColumn:@"build"  title:@"Build"  width:190 align:NSTextAlignmentLeft];
    [self addColumn:@"size"   title:@"Size"   width:70  align:NSTextAlignmentRight];
    [self addColumn:@"status" title:@"Status" width:120 align:NSTextAlignmentLeft];
    [_table setDataSource:self];
    [_table setDelegate:self];
    [_table setTarget:self];
    [_table setDoubleAction:@selector(playAction:)];
    [scroll setDocumentView:_table];
    [content addSubview:scroll];

    // Детали выбранного (title/serial + путь с обрезкой слева)
    _detail1 = [self makeLabel:NSMakeRect(16, 70, W - 32, 16)
                          font:[NSFont systemFontOfSize:11] color:[NSColor secondaryLabelColor]];
    [_detail1 setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [content addSubview:_detail1];
    _detail2 = [self makeLabel:NSMakeRect(16, 52, W - 32, 16)
                          font:[NSFont systemFontOfSize:11] color:[NSColor secondaryLabelColor]];
    [[_detail2 cell] setLineBreakMode:NSLineBreakByTruncatingHead];
    [_detail2 setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [content addSubview:_detail2];

    // Кнопки
    NSButton* browse = [self makeButton:@"Browse…" frame:NSMakeRect(16, 12, 100, 32)
                                 action:@selector(browseAction:)];
    [browse setAutoresizingMask:NSViewMaxYMargin | NSViewMaxXMargin];
    [content addSubview:browse];

    _playBtn = [self makeButton:@"Play" frame:NSMakeRect(W - 16 - 100, 12, 100, 32)
                         action:@selector(playAction:)];
    [_playBtn setKeyEquivalent:@"\r"];   // Enter = запуск (кнопка по умолчанию)
    [_playBtn setAutoresizingMask:NSViewMaxYMargin | NSViewMinXMargin];
    [content addSubview:_playBtn];

    NSButton* quit = [self makeButton:@"Quit" frame:NSMakeRect(W - 16 - 100 - 8 - 100, 12, 100, 32)
                               action:@selector(quitAction:)];
    [quit setKeyEquivalent:@"\033"];     // Esc = выход
    [quit setAutoresizingMask:NSViewMaxYMargin | NSViewMinXMargin];
    [content addSubview:quit];

    [_win center];
}

// ─── NSTableViewDataSource / Delegate (cell-based) ──────────────────────────────
- (NSInteger)numberOfRowsInTableView:(NSTableView*)t {
    return _entries ? (NSInteger)_entries->size() : 0;
}
- (id)tableView:(NSTableView*)t objectValueForTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    if (!_entries || row < 0 || row >= (NSInteger)_entries->size()) return @"";
    const launcher::Entry& e = (*_entries)[(size_t)row];
    NSString* cid = [col identifier];
    if ([cid isEqualToString:@"file"])   return [NSString stringWithUTF8String:e.file.c_str()];
    if ([cid isEqualToString:@"build"])  return [NSString stringWithUTF8String:launcher::buildLabel(e.build)];
    if ([cid isEqualToString:@"size"])   return [NSString stringWithFormat:@"%.1f MB", (double)e.size / (1024.0 * 1024.0)];
    if ([cid isEqualToString:@"status"]) return [NSString stringWithUTF8String:launcher::buildStatus(e.build)];
    return @"";
}
// Цвет всей строки по семейству ROM: ZT зелёный, ZTU жёлтый, BZT красный.
- (void)tableView:(NSTableView*)t willDisplayCell:(id)cell forTableColumn:(NSTableColumn*)col row:(NSInteger)row {
    if (![cell isKindOfClass:[NSTextFieldCell class]]) return;
    if (!_entries || row < 0 || row >= (NSInteger)_entries->size()) return;
    const launcher::Entry& e = (*_entries)[(size_t)row];
    NSColor* color = e.supported ? [NSColor controlTextColor] : [NSColor disabledControlTextColor];
    switch (launcher::buildTextTone(e.build)) {
        case launcher::TextTone::Green:  color = [NSColor colorWithCalibratedRed:0.20 green:0.72 blue:0.30 alpha:1.0]; break;
        case launcher::TextTone::Yellow: color = [NSColor colorWithCalibratedRed:0.90 green:0.70 blue:0.12 alpha:1.0]; break;
        case launcher::TextTone::Red:    color = [NSColor colorWithCalibratedRed:0.90 green:0.25 blue:0.22 alpha:1.0]; break;
        default: break;
    }
    [(NSTextFieldCell*)cell setTextColor:color];
}
- (void)tableViewSelectionDidChange:(NSNotification*)n { [self refreshDetail]; }

- (void)refreshDetail {
    NSInteger row = [_table selectedRow];
    if (row < 0 || !_entries || row >= (NSInteger)_entries->size()) {
        [_detail1 setStringValue:(_entries && _entries->empty()
            ? @"No Zero Tolerance ROMs found — use Browse… or drag & drop a .gen/.bin/.md file"
            : @"")];
        [_detail2 setStringValue:@""];
        [_playBtn setEnabled:NO];
        return;
    }
    const launcher::Entry& e = (*_entries)[(size_t)row];
    [_detail1 setStringValue:[NSString stringWithFormat:@"Title: %s    Serial: %s",
        e.title.c_str(), e.serial.c_str()]];
    [_detail2 setStringValue:[NSString stringWithFormat:@"Path: %s", e.path.c_str()]];
    [_playBtn setEnabled:YES];
}

- (void)selectRow:(int)i {
    if (i < 0 || !_entries || i >= (int)_entries->size()) return;
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
    [_table scrollRowToVisible:i];
    [self refreshDetail];
}

// ─── Действия ───────────────────────────────────────────────────────────────────
- (void)playAction:(id)sender {
    NSInteger row = [_table selectedRow];
    if (row < 0 || !_entries || row >= (NSInteger)_entries->size()) return;
    const launcher::Entry& e = (*_entries)[(size_t)row];
    if (!e.supported) {
        NSAlert* a = [[NSAlert alloc] init];
        [a setMessageText:@"This build is not supported yet"];
        [a setInformativeText:@"Only Zero Tolerance (release) and ZT Underground (partial support) can be launched right now."];
        [a addButtonWithTitle:@"OK"];
        [a runModal];
        return;
    }
    _selectedPath = e.path;
    _result = LR_PLAY;
}
- (void)quitAction:(id)sender { _result = LR_QUIT; }
- (void)cancel:(id)sender     { _result = LR_QUIT; }
- (BOOL)windowShouldClose:(id)sender { _result = LR_QUIT; return YES; }

- (void)browseAction:(id)sender {
    NSOpenPanel* p = [NSOpenPanel openPanel];
    [p setAllowsMultipleSelection:NO];
    [p setCanChooseDirectories:NO];
    [p setCanChooseFiles:YES];
    // setAllowedFileTypes устарел с macOS 12 (замена — allowedContentTypes/UTType), но работает и
    // не тянет фреймворк UniformTypeIdentifiers — гасим предупреждение точечно.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [p setAllowedFileTypes:@[@"gen", @"bin", @"md"]];
#pragma clang diagnostic pop
    [p setTitle:@"Choose a Mega Drive ROM"];
    if ([p runModal] == NSModalResponseOK) {
        NSURL* url = [[p URLs] firstObject];
        if (url) [self addRomAtPath:[url path]];
    }
}

// Добавить ROM из Browse/drag&drop: если уже в списке — выделить, иначе вставить сверху.
- (void)addRomAtPath:(NSString*)path {
    if (!_entries || !path) return;
    launcher::Entry e;
    if (!launcher::probeRom([path fileSystemRepresentation], e, /*keepUnknown=*/true)) {
        NSAlert* a = [[NSAlert alloc] init];
        [a setMessageText:@"Not a Mega Drive ROM"];
        [a setInformativeText:@"The file does not look like a Sega Mega Drive / Genesis ROM."];
        [a addButtonWithTitle:@"OK"];
        [a runModal];
        return;
    }
    for (int i = 0; i < (int)_entries->size(); ++i)
        if ((*_entries)[(size_t)i].path == e.path) { [self selectRow:i]; return; }
    _entries->insert(_entries->begin(), e);
    [_table reloadData];
    [self selectRow:0];
}
@end

// ─── Минимальное меню приложения (без MainMenu.nib): даёт ⌘Q и имя в баре ────────
static void installLauncherMenu(ZtppLauncherCtl* ctl) {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    [NSApp setMainMenu:bar];
    NSMenu* appMenu = [[NSMenu alloc] init];
    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit ZTPP"
                                                  action:@selector(cancel:) keyEquivalent:@"q"];
    [quit setTarget:ctl];
    [appMenu addItem:quit];
    [appItem setSubmenu:appMenu];
}

// ─── Точка входа C++ (объявлена в launcher.hpp) ──────────────────────────────────
namespace launcher {

std::string runNative(const std::string& preselect) {
    @autoreleasepool {
        State st = makeState(preselect);   // общий скан + предвыбор (печатает список найденных ROM)

        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        ZtppLauncherCtl* ctl = [[ZtppLauncherCtl alloc] initWithEntries:&st.entries selected:st.sel];
        installLauncherMenu(ctl);

        [app finishLaunching];
        [app activateIgnoringOtherApps:YES];
        [[ctl window] makeKeyAndOrderFront:nil];

        // Ручной модальный цикл (как startwin_run в eduke32: nextEventMatchingMask + сентинел).
        while ([ctl result] == LR_RUNNING) {
            NSEvent* ev = [app nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantFuture]
                                              inMode:NSDefaultRunLoopMode dequeue:YES];
            if (ev) [app sendEvent:ev];
            [app updateWindows];
        }

        std::string out = ([ctl result] == LR_PLAY) ? [ctl selectedPathStd] : std::string();
        [[ctl window] orderOut:nil];

        if (!out.empty()) { saveLastRom(out); std::printf("launcher: selected %s\n", out.c_str()); }
        else                std::printf("launcher: no ROM selected, exit\n");
        return out;
    }
}

} // namespace launcher
