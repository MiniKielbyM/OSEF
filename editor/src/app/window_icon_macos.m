#import <Cocoa/Cocoa.h>

#include <string.h>

void osef_editor_set_macos_app_icon(const unsigned char* pixels, int width, int height) {
    NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:width
                      pixelsHigh:height
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:width * 4
                    bitsPerPixel:32];
    if (!bitmap) return;
    memcpy([bitmap bitmapData], pixels, (size_t)width * (size_t)height * 4);

    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
    [image addRepresentation:bitmap];
    [[NSApplication sharedApplication] setApplicationIconImage:image];
    [image release];
    [bitmap release];
}
