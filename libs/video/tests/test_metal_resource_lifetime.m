/* clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_resource_lifetime.m -Wl,-dead_strip
 * -framework Metal -framework Cocoa -framework QuartzCore -o /tmp/test_metal_lifetime */
#include "../rsx_metal_backend.m"
#include <assert.h>
int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        MTLTextureDescriptor* d = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:1 height:1 mipmapped:NO];
        id<MTLTexture> original = [s_dev newTextureWithDescriptor:d];
        id<MTLTexture> replacement = [s_dev newTextureWithDescriptor:d];
        u32 old = eng_obj_add(original);
        /* A deferred draw resolves this handle only when it is encoded. */
        s_eng_rec_count = 1;
        s_eng_rec[0].kind = ENG_REC_DRAW;
        s_eng_rec[0].tex[0] = old;
        eng_obj_release(NULL, old);
        u32 fresh = eng_obj_add(replacement);
        assert(eng_obj(s_eng_rec[0].tex[0]) == original);
        assert(fresh != old);
        eng_obj_release(NULL, old); /* duplicate release must not recycle twice */
        s_eng_rec_count = 0;
        eng_collect_retired_objects();
        assert(!eng_obj(old));
        assert(eng_obj(fresh) == replacement);
        assert(eng_obj_add(original) == old);
        assert(eng_obj_add(replacement) != fresh);
        puts("Queued Metal texture handles retain their original resource");
    }
}
