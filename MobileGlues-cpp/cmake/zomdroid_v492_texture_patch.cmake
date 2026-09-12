if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "zomdroid_v492_texture_patch.cmake requires INPUT and OUTPUT")
endif()

file(READ "${INPUT}" _src)

set(_old [[TextureObject* GetOrCreateTextureObject(GLuint index) {
    if (index >= BufferObjectsVec.size()) {
        BufferObjectsVec.resize(index + 100, nullptr);
    }

    auto& obj = BufferObjectsVec[index];
    if (!obj) {
        obj = new TextureObject();
        obj->texture = index;
    }
    return obj;
}]])

set(_new [[TextureObject* GetOrCreateTextureObject(GLuint index) {
#if defined(ZOMDROID_EXPERIMENTAL)
    // PZ uses GLuint(-1) as a transient no-texture sentinel on one sprite path.
    // The old growth expression `index + 100` wrapped for names in the top 100
    // values of uint32_t and then indexed the vector with the original huge name.
    // The driver call has already happened when this helper runs, so fail only
    // frontend tracking for the proven overflow band instead of changing GL
    // submission semantics or imposing an arbitrary cap on normal sparse names.
    constexpr GLuint kGrowthSlack = 100U;
    if (index > 0xffffffffU - kGrowthSlack) {
#if defined(ZOMDROID_GL_BREADCRUMBS)
        static std::atomic<unsigned int> sparse_name_hits{0};
        const unsigned int hit = sparse_name_hits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (hit <= 8) {
            ZOMDROID_DIAGNOSTIC_LOG(
                "ZOMDROID_TEXTURE_SPARSE_NAME texture=%u action=driver_only_frontend_untracked hit=%u", index, hit);
        }
#endif
        return nullptr;
    }
#endif
    if (index >= BufferObjectsVec.size()) {
        BufferObjectsVec.resize(index + 100, nullptr);
    }

    auto& obj = BufferObjectsVec[index];
    if (!obj) {
        obj = new TextureObject();
        obj->texture = index;
    }
    return obj;
}]])

string(FIND "${_src}" "${_old}" _pos)
if(_pos EQUAL -1)
    message(FATAL_ERROR "V4.9.2 texture patch anchor not found; refusing an unverified generated source")
endif()

string(REPLACE "${_old}" "${_new}" _src "${_src}")
file(WRITE "${OUTPUT}" "${_src}")
