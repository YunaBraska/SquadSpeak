#include "ios_audio_session.hpp"

#import <AVFAudio/AVFAudio.h>

bool activateIosAudioSession(QString& error) {
    AVAudioSession* session = AVAudioSession.sharedInstance;
    NSError* nativeError = nil;
    const AVAudioSessionCategoryOptions options =
        AVAudioSessionCategoryOptionAllowBluetoothHFP | AVAudioSessionCategoryOptionAllowBluetoothA2DP;
    if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                          mode:AVAudioSessionModeDefault
                       options:options
                         error:&nativeError]) {
        error = QString::fromNSString(nativeError.localizedDescription);
        return false;
    }
    if (![session setActive:YES error:&nativeError]) {
        error = QString::fromNSString(nativeError.localizedDescription);
        return false;
    }
    return true;
}
