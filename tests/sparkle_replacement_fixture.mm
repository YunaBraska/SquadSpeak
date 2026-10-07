#import <Cocoa/Cocoa.h>
#import <Sparkle/Sparkle.h>

static void record(NSDictionary *proof) {
    NSString *path = [NSBundle.mainBundle objectForInfoDictionaryKey:@"FixtureResult"];
    NSData *data = [NSJSONSerialization dataWithJSONObject:proof options:0 error:nil];
    [data writeToFile:path atomically:YES];
}

static void rejected(NSError *error) {
    NSMutableArray *codes = [NSMutableArray array];
    for (NSError *current = error; current; current = current.userInfo[NSUnderlyingErrorKey]) {
        if ([current.domain isEqualToString:SUSparkleErrorDomain]) [codes addObject:@(current.code)];
    }
    NSLog(@"Update rejected: %@", error);
    record(@{@"event": @"rejected", @"codes": codes});
    dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
}

@interface FixtureDriver : NSObject <SPUUserDriver>
@end
@implementation FixtureDriver
- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest *)request reply:(void (^)(SUUpdatePermissionResponse *))reply { reply([[SUUpdatePermissionResponse alloc] initWithAutomaticUpdateChecks:YES sendSystemProfile:NO]); }
- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation {}
- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem *)item state:(SPUUserUpdateState *)state reply:(void (^)(SPUUserUpdateChoice))reply { reply(SPUUserUpdateChoiceInstall); }
- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData *)data {}
- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError *)error { rejected(error); }
- (void)showUpdateNotFoundWithError:(NSError *)error acknowledgement:(void (^)(void))acknowledgement { acknowledgement(); rejected(error); }
- (void)showUpdaterError:(NSError *)error acknowledgement:(void (^)(void))acknowledgement { acknowledgement(); rejected(error); }
- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {}
- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)length {}
- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length {}
- (void)showDownloadDidStartExtractingUpdate {}
- (void)showExtractionReceivedProgress:(double)progress {}
- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply { reply(SPUUserUpdateChoiceInstall); }
- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)terminated retryTerminatingApplication:(void (^)(void))retry { if (!terminated) retry(); }
- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched acknowledgement:(void (^)(void))acknowledgement { acknowledgement(); }
- (void)dismissUpdateInstallation {}
- (void)showUpdateInFocus {}
@end

int main() {
    @autoreleasepool {
        NSApplication *application = NSApplication.sharedApplication;
        application.activationPolicy = NSApplicationActivationPolicyProhibited;
        NSBundle *bundle = NSBundle.mainBundle;
        NSString *markerPath = [bundle pathForResource:@"replacement-proof" ofType:@"txt"];
        if (markerPath) {
            NSString *marker = [NSString stringWithContentsOfFile:markerPath encoding:NSUTF8StringEncoding error:nil];
            record(@{@"event": @"relaunched", @"version": [bundle objectForInfoDictionaryKey:@"CFBundleVersion"], @"marker": marker});
            return 0;
        }
        FixtureDriver *driver = [FixtureDriver new];
        SPUUpdater *updater = [[SPUUpdater alloc] initWithHostBundle:bundle applicationBundle:bundle userDriver:driver delegate:nil];
        NSError *error = nil;
        if (![updater startUpdater:&error]) { rejected(error); return 2; }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            record(@{@"event": @"timeout"});
            [application terminate:nil];
        });
        [updater checkForUpdates];
        [application run];
    }
    return 0;
}
