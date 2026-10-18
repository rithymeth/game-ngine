#import <UIKit/UIKit.h>

#include <aether/core/log.h>
#include <aether/core/version.h>

@interface AetherAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation AetherAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    (void)application;
    (void)launchOptions;

    auto& logger = aether::Logger::Instance();
    logger.SetStdout(false);
    logger.Log(aether::LogLevel::Info, "iOS", "Aether Engine native core started");

    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UIViewController* root = [[UIViewController alloc] init];
    root.view.backgroundColor = UIColor.systemBackgroundColor;

    UILabel* status = [[UILabel alloc] initWithFrame:root.view.bounds];
    status.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    status.textAlignment = NSTextAlignmentCenter;
    status.numberOfLines = 0;
    status.text = [NSString stringWithFormat:@"Aether Engine %@\nNative core ready",
                                             [NSString stringWithUTF8String:aether::kEngineVersion]];
    [root.view addSubview:status];

    self.window.rootViewController = root;
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(AetherAppDelegate.class));
    }
}
