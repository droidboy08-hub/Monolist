#include "loginitem.h"

#include <QString>

#import <Foundation/Foundation.h>
#import <ServiceManagement/ServiceManagement.h>

namespace MacLoginItem {

bool turnedOff(const QString &agentPath)
{
    if (@available(macOS 13.0, *)) {
        NSURL *url = [NSURL fileURLWithPath:agentPath.toNSString()];
        // Background Task Management's answer for an agent in the user's
        // LaunchAgents: enabled, or waiting on the user's approval once they
        // turned it off. Anything else (never seen, not found) is not a no.
        return [SMAppService statusForLegacyURL:url] == SMAppServiceStatusRequiresApproval;
    }
    return false;
}

} // namespace MacLoginItem
