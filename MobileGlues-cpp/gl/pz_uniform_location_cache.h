// MobileGlues - gl/pz_uniform_location_cache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1.

#ifndef MOBILEGLUES_PZ_UNIFORM_LOCATION_CACHE_H
#define MOBILEGLUES_PZ_UNIFORM_LOCATION_CACHE_H

#include <GL/gl.h>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

class mg_pz_uniform_location_cache {
  public:
    bool lookup(unsigned long long context_id, GLuint program, const char* name, GLint* location) {
        bind_context(context_id);
        if (context_id == 0 || program == 0 || name == nullptr || location == nullptr) return false;
        const auto program_it = programs_.find(program);
        if (program_it == programs_.end()) return false;
        const auto location_it = program_it->second.find(name);
        if (location_it == program_it->second.end()) return false;
        *location = location_it->second;
        return true;
    }

    bool store(unsigned long long context_id, GLuint program, const char* name, GLint location) {
        bind_context(context_id);
        // A negative location can represent either an inactive name or an API
        // error. Let the driver see it again so cached misses never suppress GL
        // errors and a later valid link can recover normally.
        if (context_id == 0 || program == 0 || name == nullptr || location < 0) return false;
        programs_[program].insert_or_assign(name, location);
        return true;
    }

    void forget_program(GLuint program) { programs_.erase(program); }

  private:
    struct transparent_string_hash {
        using is_transparent = void;

        size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
        size_t operator()(const std::string& value) const noexcept { return operator()(std::string_view(value)); }
        size_t operator()(const char* value) const noexcept { return operator()(std::string_view(value)); }
    };

    using location_map =
        std::unordered_map<std::string, GLint, transparent_string_hash, std::equal_to<>>;

    void bind_context(unsigned long long context_id) {
        if (context_id_ == context_id) return;
        programs_.clear();
        context_id_ = context_id;
    }

    unsigned long long context_id_ = 0;
    std::unordered_map<GLuint, location_map> programs_;
};

#endif // MOBILEGLUES_PZ_UNIFORM_LOCATION_CACHE_H
