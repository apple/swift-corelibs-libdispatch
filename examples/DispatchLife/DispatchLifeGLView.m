/*
 * Copyright (c) 2009-2009 Apple Inc.  All rights reserved.
 *
 * @APPLE_DTS_LICENSE_HEADER_START@
 * 
 * IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc.
 * ("Apple") in consideration of your agreement to the following terms, and your
 * use, installation, modification or redistribution of this Apple software
 * constitutes acceptance of these terms.  If you do not agree with these terms,
 * please do not use, install, modify or redistribute this Apple software.
 * 
 * In consideration of your agreement to abide by the following terms, and
 * subject to these terms, Apple grants you a personal, non-exclusive license,
 * under Apple's copyrights in this original Apple software (the "Apple Software"),
 * to use, reproduce, modify and redistribute the Apple Software, with or without
 * modifications, in source and/or binary forms; provided that if you redistribute
 * the Apple Software in its entirety and without modifications, you must retain
 * this notice and the following text and disclaimers in all such redistributions
 * of the Apple Software.  Neither the name, trademarks, service marks or logos of
 * Apple Computer, Inc. may be used to endorse or promote products derived from
 * the Apple Software without specific prior written permission from Apple.  Except
 * as expressly stated in this notice, no other rights or licenses, express or
 * implied, are granted by Apple herein, including but not limited to any patent
 * rights that may be infringed by your derivative works or by other works in
 * which the Apple Software may be incorporated.
 * 
 * The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO
 * WARRANTIES, EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED
 * WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE OR IN
 * COMBINATION WITH YOUR PRODUCTS. 
 * 
 * IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * ARISING IN ANY WAY OUT OF THE USE, REPRODUCTION, MODIFICATION AND/OR
 * DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER UNDER THEORY OF
 * CONTRACT, TORT (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN IF
 * APPLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * @APPLE_DTS_LICENSE_HEADER_END@
 */

#import "DispatchLifeGLView.h"

#import <OpenGL/OpenGL.h>

#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/glu.h>

#include <OpenGL/CGLCurrent.h>
#include <OpenGL/CGLContext.h>

extern size_t grid_x_size;
extern size_t grid_y_size;

@implementation DispatchLifeGLView

#define CELL_WIDTH 8
#define CELL_HEIGHT 8

- (void)goFullScreen:(NSOpenGLView*)view {
	NSOpenGLPixelFormatAttribute attrs[] =
	{
		NSOpenGLPFAFullScreen,
		
		NSOpenGLPFAScreenMask, CGDisplayIDToOpenGLDisplayMask(kCGDirectMainDisplay),

		NSOpenGLPFAAccelerated,
		NSOpenGLPFANoRecovery,
		NSOpenGLPFADoubleBuffer,
		0
	};
	NSOpenGLPixelFormat* pixFmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
	
    NSOpenGLContext* screen = [[NSOpenGLContext alloc] initWithFormat:pixFmt shareContext:[view openGLContext]];
	
    CGDisplayErr err = CGCaptureAllDisplays();
    if (err != CGDisplayNoErr) {
        [screen release];
        return;
    }
	
    [screen setFullScreen];
    [screen makeCurrentContext];

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);
    [screen flushBuffer];
    glClear(GL_COLOR_BUFFER_BIT);
    [screen flushBuffer];
}


- (id)initWithFrame:(NSRect)frame {
	NSOpenGLPixelFormatAttribute attrs[] =
	{
		NSOpenGLPFAAccelerated,
		NSOpenGLPFANoRecovery,
		NSOpenGLPFADoubleBuffer,
		0
	};
	NSOpenGLPixelFormat* pixFmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];

	self = [super initWithFrame:frame pixelFormat:pixFmt];
    if (self) {

		[[self openGLContext] makeCurrentContext];
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glClearColor(1.0, 1.0, 1.0, 1.0);
		glColor4f(1.0, 1.0, 1.0, 1.0);
		glEnable(GL_RASTER_POSITION_UNCLIPPED_IBM);
		glDisable(GL_DITHER);

		grid_x_size = 128;
		grid_y_size = 96;

		self->grid = init_grid(grid_x_size, grid_y_size);
		size_t image_size = grid_x_size * grid_y_size * sizeof(uint32_t);
		self->image = malloc(image_size);
		memset(self->image, 0xFF, image_size);

		[self adjustGLViewBounds];

		[[NSTimer scheduledTimerWithTimeInterval: (1.0f / 15.0) target: self selector:@selector(drawRect:) userInfo:self repeats:true] retain];
		
	}
    return self;
}

- (void)drawRect:(NSRect)rect {
	[[self openGLContext] makeCurrentContext];
	
	glClear(GL_COLOR_BUFFER_BIT);

	NSRect bounds = [self bounds];
	glRasterPos2f(-bounds.size.width/2, -bounds.size.height/2);
	glPixelZoom(bounds.size.width/grid_x_size, bounds.size.height/grid_y_size);

	const int width = grid_x_size;
	const int height = grid_y_size;

	int x, y;
	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
			int i = y * width + x;
			switch (get_grid_display_char(grid, x, y)) {
				case '.':
					image[i] = 0xCCCCCCFF;
					break;
				case '#':
					image[i] = 0x000000FF;
					break;
				case ' ':
					image[i] = 0xFFFFFFFF;
					break;
				default:
					image[i] = 0x0000FFFF;
					break;
			}
		}
	}

	glDrawPixels(width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, image);

	glFinish();

	[[self openGLContext] flushBuffer];

}

- (void)adjustGLViewBounds
{
	[[self openGLContext] makeCurrentContext];
	[[self openGLContext] update];
	
	NSRect rect = [self bounds];
	
	glViewport(0, 0, (GLint) rect.size.width, (GLint) rect.size.height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	gluOrtho2D(-(rect.size.width/2), rect.size.width/2, -(rect.size.height/2), rect.size.height/2);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity(); 

	[self setNeedsDisplay:true];
}

- (void)update  // moved or resized
{
	[super update];
	[self adjustGLViewBounds];
}

- (void)reshape	// scrolled, moved or resized
{
	[super reshape];
	[self adjustGLViewBounds];
}

@end
