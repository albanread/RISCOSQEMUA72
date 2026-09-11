// Does a Metal fill beat memset for the rectangles RISC OS actually asks for?
//
//   clang -fobjc-arc -O2 -framework Foundation -framework Metal \
//         fillbench.m -o fillbench && ./fillbench
//
// The question came up when riscos-blitter was written: on Apple silicon the
// GPU shares memory with the CPU, so it can fill the guest's framebuffer in
// place with no transfer at all -- guest RAM here stands in as a page-aligned
// block wrapped by newBufferWithBytesNoCopy, exactly as QEMU's RAMBlock can
// be.  There is no copy to object to, so the objection has to be measured.
//
// On an M4 (2026-09-11):
//
//   full screen 9 MB   per-row memset 118us   one memset 68us
//                      Metal sync 510us       Metal pipelined 132us
//   window     1.9 MB  per-row memset  49us   Metal sync 232us   pipelined 27us
//   icon row     6 KB  per-row memset 0.1us   Metal sync 149us   pipelined 20us
//
// The cost is latency, not bandwidth: about 150us to dispatch and wait.  It
// cannot be pipelined away because the blit has to be synchronous -- the
// guest plots text and sprites into the same framebuffer on the next
// instruction.  Against a real redraw, thirteen small rectangles and two
// full-screen clears, that is ~81us of memset against ~2.6ms of Metal.
//
// What it did show is that contiguous rows want one big write, not one per
// row; blit_contiguous() in hw/misc/riscos_blitter.c came from this.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <string.h>

static double to_us(uint64_t t) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)t * tb.numer / tb.denom / 1000.0;
}

static NSString *kernelSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void fillrect(device uint4 *fb [[buffer(0)]],\n"
"                     constant uint4 &g [[buffer(1)]],\n"   // quads/row, rows, stride quads, colour
"                     uint2 gid [[thread_position_in_grid]]) {\n"
"    if (gid.x >= g.x || gid.y >= g.y) return;\n"
"    fb[gid.y * g.z + gid.x] = uint4(g.w, g.w, g.w, g.w);\n"
"}\n";

int main(void) { @autoreleasepool {
    const uint32_t W = 1920, H = 1200, BPP = 4;
    const size_t pitch = W * BPP;
    size_t fbsize = (size_t)pitch * H;
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    fbsize = (fbsize + pagesz - 1) & ~(pagesz - 1);

    void *ram = NULL;
    if (posix_memalign(&ram, pagesz, fbsize)) return 1;
    memset(ram, 0, fbsize);

    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    printf("device: %s\n", dev.name.UTF8String);
    printf("has unified memory: %s\n", dev.hasUnifiedMemory ? "yes" : "no");
    printf("page size: %zu\n\n", pagesz);

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kernelSrc options:nil error:&err];
    if (!lib) { printf("shader: %s\n", err.description.UTF8String); return 1; }
    id<MTLComputePipelineState> pso =
        [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fillrect"] error:&err];
    id<MTLCommandQueue> q = [dev newCommandQueue];

    // The whole point: no copy, the GPU addresses the same pages.
    id<MTLBuffer> buf = [dev newBufferWithBytesNoCopy:ram length:fbsize
                                              options:MTLResourceStorageModeShared
                                          deallocator:nil];
    printf("wrapped guest RAM with no copy: %s\n\n", buf ? "yes" : "NO");
    if (!buf) return 1;

    struct { uint32_t wpr, rows, stridew, colour; } g;

    struct { const char *name; uint32_t w, h; } cases[] = {
        { "full screen 1920x1200", 1920, 1200 },
        { "window       800x600",   800,  600 },
        { "icon bar row  91x17",     91,   17 },   // 364 bytes / 4
    };

    for (int c = 0; c < 3; c++) {
        uint32_t w = cases[c].w, h = cases[c].h;
        size_t bytes = (size_t)w * h * BPP;
        int iters = (bytes > 1u<<20) ? 200 : 2000;

        // 1. per-row memset, which is what riscos-blitter does today
        uint64_t t0 = mach_absolute_time();
        for (int i = 0; i < iters; i++)
            for (uint32_t y = 0; y < h; y++)
                memset((char *)ram + y * pitch, i, w * BPP);
        double rowms = to_us(mach_absolute_time() - t0) / iters;

        // 2. one memset, when the rows are contiguous
        double flat = 0;
        if (w * BPP == pitch) {
            t0 = mach_absolute_time();
            for (int i = 0; i < iters; i++) memset(ram, i, bytes);
            flat = to_us(mach_absolute_time() - t0) / iters;
        }

        // 3. Metal, synchronous: the guest cannot continue until it lands
        g.wpr = w / 4; g.rows = h; g.stridew = (uint32_t)(pitch / 16); g.colour = 0x00ff00ff;
        MTLSize grid = MTLSizeMake(w / 4, h, 1);
        NSUInteger tw = MIN((NSUInteger)32, pso.threadExecutionWidth);
        MTLSize tg = MTLSizeMake(tw, MIN((NSUInteger)8, pso.maxTotalThreadsPerThreadgroup / tw), 1);

        for (int warm = 0; warm < 20; warm++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:pso];
            [e setBuffer:buf offset:0 atIndex:0];
            [e setBytes:&g length:sizeof(g) atIndex:1];
            [e dispatchThreads:grid threadsPerThreadgroup:tg];
            [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        }
        t0 = mach_absolute_time();
        for (int i = 0; i < iters; i++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:pso];
            [e setBuffer:buf offset:0 atIndex:0];
            [e setBytes:&g length:sizeof(g) atIndex:1];
            [e dispatchThreads:grid threadsPerThreadgroup:tg];
            [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        }
        double gpu = to_us(mach_absolute_time() - t0) / iters;

        printf("%-22s %8zu KB   per-row memset %8.1f us", cases[c].name, bytes/1024, rowms);
        if (flat) printf("   one memset %8.1f us", flat);
        // submit without waiting: the floor if correctness did not need a sync
        t0 = mach_absolute_time();
        id<MTLCommandBuffer> last = nil;
        for (int i = 0; i < iters; i++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:pso];
            [e setBuffer:buf offset:0 atIndex:0];
            [e setBytes:&g length:sizeof(g) atIndex:1];
            [e dispatchThreads:grid threadsPerThreadgroup:tg];
            [e endEncoding]; [cb commit]; last = cb;
        }
        [last waitUntilCompleted];
        double gpuasync = to_us(mach_absolute_time() - t0) / iters;
        printf("   Metal sync %8.1f us   Metal pipelined %8.1f us\n", gpu, gpuasync);
    }
    return 0;
}}
