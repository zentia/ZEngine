// =============================================================================
// ZEngine iOS Player — UIApplicationDelegate + CADisplayLink
// -----------------------------------------------------------------------------
// iOS 上没有 android_main 那样的统一入口：宿主是标准的 UIKit App，引擎被
// CADisplayLink 每帧驱动一次（等价于 Android 侧的 ALooper 循环）。
//
// 这里只做四件事，业务逻辑一律在 Player 壳里：
//   1. 建 UIWindow + 一个 UIView 作为绘制表面（MetalRHI 会在它上面挂
//      CAMetalLayer，见 MetalRHI.mm 的 AttachMetalLayer）
//   2. 把 .app 里的 zengine/ 资源目录作为项目根目录启动引擎
//   3. CADisplayLink 每帧调 ZPlayer_Tick
//   4. 生命周期回调映射成 ZPlayer_Pause / Resume / SetFocus / Shutdown
//
// 与 Android 侧共用的是同一套 ZPlayer_* 接口，启动序列完全一致。
// =============================================================================

#import <QuartzCore/CADisplayLink.h>
#import <UIKit/UIKit.h>

#include <string>

#include "Player/PlayerShell.h"

namespace
{
    /// 项目内容在 .app 里的位置（由 zbuild_ios.py 拷进 bundle 的 Resources）。
    ///
    /// ⚠️ 名字**不能**叫 "zengine"：可执行文件的 OUTPUT_NAME 是 "ZEngine"，
    /// 而 macOS 默认卷宗是**大小写不敏感**的，于是 `<app>/zengine` 与
    /// `<app>/ZEngine` 被当成同一条路径 —— 拷贝资源时 mkdir 得到
    /// ENOTDIR / EEXIST（ninja 报法：`mkdir(.../ZEngine.app/zengine/Build):
    /// Not a directory`，看上去完全不像大小写问题），带项目内容的 iOS 构建
    /// 在 Mac 上根本编不出来。iOS 设备本身是大小写敏感的，所以这个坑只在
    /// 构建宿主机上发作，真机上永远不会复现。
    constexpr const char* k_bundled_project_dir = "zengine_project";
}  // namespace

// ---------------------------------------------------------------------------
// 绘制表面（MetalRHI 会在它上面挂 CAMetalLayer）。
//
// iOS 的 UITouch 没有稳定的整数 id（Android 有 AMotionEvent_getPointerId），
// 所以宿主得自己分配：按下时给一个自增 id，抬起时释放表项。
//
// key 用 UITouch 的地址，而地址会被系统复用，因此 id 只增不减 —— 保证同一根
// 手指在它自己的生命周期内 id 唯一（复用出来的新手指拿到的是新 id）。
// ---------------------------------------------------------------------------
@interface ZEngineRenderView : UIView

@property (nonatomic, strong) NSMutableDictionary<NSValue*, NSNumber*>* touchIds;
@property (nonatomic, assign) int nextTouchId;

@end

@implementation ZEngineRenderView

- (instancetype)initWithFrame:(CGRect)frame
{
    self = [super initWithFrame:frame];
    if (self != nil)
    {
        _touchIds    = [[NSMutableDictionary alloc] init];
        _nextTouchId = 0;
        // 多指：一根手指走摇杆，另一根转视角，是移动端的基本操作。
        self.multipleTouchEnabled = YES;
    }
    return self;
}

- (int)pointerIdForTouch:(UITouch*)touch
{
    NSValue*  key      = [NSValue valueWithPointer:(__bridge const void*)touch];
    NSNumber* existing = self.touchIds[key];
    if (existing != nil)
    {
        return existing.intValue;
    }
    const int newId = self.nextTouchId++;
    self.touchIds[key] = @(newId);
    return newId;
}

- (void)releasePointerIdForTouch:(UITouch*)touch
{
    [self.touchIds removeObjectForKey:[NSValue valueWithPointer:(__bridge const void*)touch]];
}

- (void)dispatchTouches:(NSSet<UITouch*>*)touches action:(int)action
{
    for (UITouch* touch in touches)
    {
        const CGPoint location = [touch locationInView:self];
        ZPlayer_Touch([self pointerIdForTouch:touch],
                      action,
                      static_cast<float>(location.x),
                      static_cast<float>(location.y));

        if (action == ZPlayer_Touch_Up || action == ZPlayer_Touch_Cancel)
        {
            [self releasePointerIdForTouch:touch];
        }
    }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self dispatchTouches:touches action:ZPlayer_Touch_Down];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self dispatchTouches:touches action:ZPlayer_Touch_Move];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    [self dispatchTouches:touches action:ZPlayer_Touch_Up];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
    // 系统接管了手指（来电、上滑回主屏、下拉通知栏）。必须发 Cancel：只发 Up
    // 会丢语义，而不发则引擎一直以为手指还按着 —— 回到前台就变成"角色自己在走"。
    [self dispatchTouches:touches action:ZPlayer_Touch_Cancel];
}

@end

@interface ZEngineAppDelegate : UIResponder <UIApplicationDelegate>

@property (nonatomic, strong) UIWindow*      window;
@property (nonatomic, strong) UIView*        renderView;
@property (nonatomic, strong) CADisplayLink* displayLink;
@property (nonatomic, assign) BOOL           bootFailed;

@end

@implementation ZEngineAppDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
    [self setupWindow];
    [self bootEngine];
    if (self.bootFailed)
    {
        return YES;  // 错误已经画在屏幕上，不再启动帧循环
    }
    [self startFrameLoop];
    return YES;
}

- (void)setupWindow
{
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];

    UIViewController* controller = [[UIViewController alloc] init];
    self.renderView = [[ZEngineRenderView alloc] initWithFrame:self.window.bounds];
    // drawable 要按物理像素算，否则 Retina 上是糊的。
    self.renderView.contentScaleFactor = [UIScreen mainScreen].nativeScale;
    self.renderView.backgroundColor    = UIColor.blackColor;
    self.renderView.autoresizingMask   = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    controller.view = self.renderView;

    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];
}

- (void)bootEngine
{
    NSString* resourcePath = [[NSBundle mainBundle] resourcePath];
    NSString* projectRoot  = [resourcePath stringByAppendingPathComponent:@(k_bundled_project_dir)];
    NSString* documents    = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                                  NSUserDomainMask, YES) firstObject];

    const CGFloat scale  = self.renderView.contentScaleFactor > 0.0
                               ? self.renderView.contentScaleFactor
                               : 1.0;
    const CGRect  bounds = self.renderView.bounds;

    ZPlayerBootDesc desc {};
    desc.project_path   = [projectRoot UTF8String];
    desc.writable_dir   = (documents != nil) ? [documents UTF8String] : "";
    desc.native_window  = (__bridge void*)self.renderView;
    desc.asset_manager  = nullptr;  // iOS 用不到 AAssetManager
    desc.preferred_rhi  = ZPlayer_RHI_Default;
    desc.surface_width  = static_cast<int>(bounds.size.width * scale);
    desc.surface_height = static_cast<int>(bounds.size.height * scale);

    const ZPlayerResult result = ZPlayer_Boot(&desc);
    if (result != ZPlayer_OK)
    {
        self.bootFailed = YES;
        [self showBootFailure:result projectPath:projectRoot];
    }
}

- (void)startFrameLoop
{
    self.displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(frameTick:)];
    // iOS 15+ 可以指定帧率；这里用默认的屏幕刷新率。
    [self.displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)frameTick:(CADisplayLink*)link
{
    // targetTimestamp - timestamp 是这一帧的真实时长（iOS 10+）；掉帧时它比
    // duration 大，用它做 delta 动画才不会走慢。取不到就退回 duration。
    float delta = static_cast<float>(link.targetTimestamp - link.timestamp);
    if (!(delta > 0.0f))
    {
        delta = static_cast<float>(link.duration);
    }

    if (ZPlayer_Tick(delta) == 0)
    {
        [self.displayLink invalidate];
        self.displayLink = nil;
        ZPlayer_Shutdown();
    }
}

- (void)showBootFailure:(ZPlayerResult)result projectPath:(NSString*)projectPath
{
    UILabel* label = [[UILabel alloc] initWithFrame:self.window.bounds];
    label.numberOfLines   = 0;
    label.textColor       = UIColor.whiteColor;
    label.backgroundColor = UIColor.blackColor;
    label.textAlignment   = NSTextAlignmentCenter;
    label.text = [NSString stringWithFormat:
                  @"ZEngine failed to start (code %d)\n\n%@",
                  static_cast<int>(result), projectPath];

    UIViewController* controller = [[UIViewController alloc] init];
    controller.view = label;
    self.window.rootViewController = controller;
    [self.window makeKeyAndVisible];

    NSLog(@"[ZEngine] boot failed (%d), project path = %@",
          static_cast<int>(result), projectPath);
}

- (void)applicationWillResignActive:(UIApplication*)application
{
    ZPlayer_SetFocus(0);
    ZPlayer_Pause();
}

- (void)applicationDidBecomeActive:(UIApplication*)application
{
    ZPlayer_Resume();
    ZPlayer_SetFocus(1);
}

- (void)applicationDidEnterBackground:(UIApplication*)application
{
    ZPlayer_Pause();
}

- (void)applicationWillEnterForeground:(UIApplication*)application
{
    ZPlayer_Resume();
}

- (void)applicationWillTerminate:(UIApplication*)application
{
    [self.displayLink invalidate];
    self.displayLink = nil;
    ZPlayer_Shutdown();
}

@end

int main(int argc, char* argv[])
{
    @autoreleasepool
    {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([ZEngineAppDelegate class]));
    }
}
