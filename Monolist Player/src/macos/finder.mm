#include "finder.h"

#include "oggtags.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>

#include <sys/xattr.h>

namespace MacFinder {

namespace {

// The icon's size in pixels. The Finder's lists, columns and usual icon
// sizes draw an icon at 128 points or less (256 px on a Retina screen); only
// the largest icon sizes draw it bigger, a little soft. 512 px would keep
// 650 KB beside each song instead of 200.
constexpr int kIconPixels = 256;

} // namespace

bool setCover(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1String("opus") && suffix != QLatin1String("ogg") && suffix != QLatin1String("oga"))
        return false;

    OggTags tags;
    const QByteArray file = QFile::encodeName(path);
    bool done = false;
    @autoreleasepool {
        CGImageRef cover = nullptr;
        if (ogg_tags_read(file.constData(), OGG_TAGS_PICTURE, &tags) == 0 && tags.picture) {
            NSData *data = [NSData dataWithBytes:tags.picture length:tags.picture_size];
            CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
            if (source) {
                // Decoded at about the icon's size, turned as its EXIF says.
                NSDictionary *options = @{
                    (__bridge NSString *)kCGImageSourceCreateThumbnailFromImageAlways: @YES,
                    (__bridge NSString *)kCGImageSourceCreateThumbnailWithTransform: @YES,
                    (__bridge NSString *)kCGImageSourceThumbnailMaxPixelSize: @(kIconPixels),
                };
                cover = CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)options);
                CFRelease(source);
            }
        }
        ogg_tags_free(&tags);

        if (cover && CGImageGetWidth(cover) > 0 && CGImageGetHeight(cover) > 0) {
            // The whole cover, as large as the square allows, centred: a
            // 16:9 picture keeps its shape, with nothing around it.
            const double width = double(CGImageGetWidth(cover));
            const double height = double(CGImageGetHeight(cover));
            const double fit = std::min(kIconPixels / width, kIconPixels / height);
            const CGRect frame = CGRectMake(std::round((kIconPixels - width * fit) / 2),
                                            std::round((kIconPixels - height * fit) / 2),
                                            std::round(width * fit), std::round(height * fit));
            CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef context = CGBitmapContextCreate(nullptr, kIconPixels, kIconPixels, 8, 0, space,
                                                         kCGImageAlphaPremultipliedLast);
            CGColorSpaceRelease(space);
            if (context) {
                CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
                CGContextDrawImage(context, frame, cover);
                CGImageRef square = CGBitmapContextCreateImage(context);
                CGContextRelease(context);
                if (square) {
                    NSImage *icon = [[NSImage alloc] initWithCGImage:square
                                                                size:NSMakeSize(kIconPixels, kIconPixels)];
                    CGImageRelease(square);
                    // Without the QuickDraw-era copies, which only old
                    // systems read.
                    done = [[NSWorkspace sharedWorkspace] setIcon:icon
                                                          forFile:path.toNSString()
                                                          options:NSExcludeQuickDrawElementsIconCreationOption];
                }
            }
        }
        if (cover)
            CGImageRelease(cover);
    }
    return done;
}

bool hasCustomIcon(const QString &path)
{
    // The Finder's own flag (kHasCustomIcon in the Finder info's flags, big
    // endian at byte 8), and an icon in the resource fork for it to show.
    // NSURLCustomIconKey answers nil for these files whatever they hold.
    const QByteArray file = QFile::encodeName(path);
    unsigned char info[32] = {};
    if (getxattr(file.constData(), XATTR_FINDERINFO_NAME, info, sizeof info, 0, 0) != sizeof info)
        return false;
    const bool flagged = (info[8] << 8 | info[9]) & 0x0400;
    return flagged && getxattr(file.constData(), XATTR_RESOURCEFORK_NAME, nullptr, 0, 0, 0) > 0;
}

namespace {

QString importerPath()
{
    const QString path = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                         + QStringLiteral("/../Library/Spotlight/MonolistOgg.mdimporter"));
    return QFileInfo::exists(path) ? path : QString();
}

} // namespace

QString spotlightImporter()
{
    const QString path = importerPath();
    if (path.isEmpty())
        return QString();
    @autoreleasepool {
        NSBundle *bundle = [NSBundle bundleWithPath:path.toNSString()];
        NSString *build = bundle.infoDictionary[@"CFBundleVersion"];
        return QString::fromNSString(build ?: @"0") + QLatin1Char(' ') + path;
    }
}

void announceSpotlightImporter()
{
    const QString path = importerPath();
    if (!path.isEmpty())
        QProcess::startDetached(QStringLiteral("/usr/bin/mdimport"), { QStringLiteral("-r"), path });
}

} // namespace MacFinder
