/*
 * This file is part of LSPosed.
 *
 * LSPosed is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LSPosed is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LSPosed.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2020 EdXposed Contributors
 * Copyright (C) 2021 LSPosed Contributors
 */

#include <jni.h>
#include <android-base/macros.h>
#include "JNIHelper.h"
#include "jni/config_manager.h"
#include "jni/art_class_linker.h"
#include "jni/yahfa.h"
#include "jni/resources_hook.h"
#include <dl_util.h>
#include <art/runtime/jni_env_ext.h>
#include <android-base/strings.h>
#include "jni/pending_hooks.h"
#include <sandhook.h>
#include <fstream>
#include <sstream>
#include "context.h"
#include "config_manager.h"
#include "native_hook.h"
#include "jni/logger.h"
#include "jni/native_api.h"
#include "service.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-value"

namespace lspd {
    namespace fs = std::filesystem;

    constexpr int FIRST_ISOLATED_UID = 99000;
    constexpr int LAST_ISOLATED_UID = 99999;
    constexpr int FIRST_APP_ZYGOTE_ISOLATED_UID = 90000;
    constexpr int LAST_APP_ZYGOTE_ISOLATED_UID = 98999;
    constexpr int SHARED_RELRO_UID = 1037;
    constexpr int PER_USER_RANGE = 100000;

    void Context::CallOnPostFixupStaticTrampolines(void *class_ptr) {
        if (UNLIKELY(!class_ptr || !class_linker_class_ || !post_fixup_static_mid_)) {
            return;
        }

        JNIEnv *env;
        vm_->GetEnv((void **) (&env), JNI_VERSION_1_4);
        art::JNIEnvExt env_ext(env);
        ScopedLocalRef clazz(env, env_ext.NewLocalRefer(class_ptr));
        if (clazz != nullptr) {
            JNI_CallStaticVoidMethod(env, class_linker_class_, post_fixup_static_mid_, clazz.get());
        }
    }

    void Context::PreLoadDex(const fs::path &dex_path) {
        if (LIKELY(!dex.empty())) return;

        std::ifstream is(dex_path, std::ios::binary);
        if (!is.good()) {
            LOGE("Cannot load path %s", dex_path.c_str());
            return;
        }
        dex.assign(std::istreambuf_iterator<char>(is),
                   std::istreambuf_iterator<char>());
        LOGI("Loaded %s with size %zu", dex_path.c_str(), dex.size());
    }

    void Context::LoadDex(JNIEnv *env) {
        jclass classloader = JNI_FindClass(env, "java/lang/ClassLoader");
        jmethodID getsyscl_mid = JNI_GetStaticMethodID(
                env, classloader, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
        jobject sys_classloader = JNI_CallStaticObjectMethod(env, classloader, getsyscl_mid);
        if (UNLIKELY(!sys_classloader)) {
            LOGE("getSystemClassLoader failed!!!");
            return;
        }
        jclass in_memory_classloader = JNI_FindClass(env, "dalvik/system/InMemoryDexClassLoader");
        jmethodID initMid = JNI_GetMethodID(env, in_memory_classloader, "<init>",
                                            "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        jclass byte_buffer_class = JNI_FindClass(env, "java/nio/ByteBuffer");
        auto dex_buffer = env->NewDirectByteBuffer(reinterpret_cast<void *>(dex.data()),
                                                   dex.size());
        jobject my_cl = JNI_NewObject(env, in_memory_classloader, initMid,
                                      dex_buffer, sys_classloader);
        env->DeleteLocalRef(classloader);
        env->DeleteLocalRef(sys_classloader);
        env->DeleteLocalRef(in_memory_classloader);
        env->DeleteLocalRef(byte_buffer_class);

        if (UNLIKELY(my_cl == nullptr)) {
            LOGE("InMemoryDexClassLoader creation failed!!!");
            return;
        }

        inject_class_loader_ = env->NewGlobalRef(my_cl);

        env->DeleteLocalRef(my_cl);

        env->GetJavaVM(&vm_);

        // Make sure config manager is always working
        RegisterConfigManagerMethods(env);
    }

    void Context::Init(JNIEnv *env) {
        class_linker_class_ = (jclass) env->NewGlobalRef(
                FindClassFromCurrentLoader(env, kClassLinkerClassName));
        post_fixup_static_mid_ = JNI_GetStaticMethodID(env, class_linker_class_,
                                                       "onPostFixupStaticTrampolines",
                                                       "(Ljava/lang/Class;)V");

        entry_class_ = (jclass) (env->NewGlobalRef(
                FindClassFromLoader(env, GetCurrentClassLoader(), kEntryClassName)));

        RegisterLogger(env);
        RegisterEdxpResourcesHook(env);
        RegisterArtClassLinker(env);
        RegisterEdxpYahfa(env);
        RegisterPendingHooks(env);
        RegisterNativeAPI(env);

//        variant_ = Variant(ConfigManager::GetInstance()->GetVariant());
//        LOGI("LSP Variant: %d", variant_);
//
//        if (variant_ == SANDHOOK) {
//            //for SandHook variant
//            ScopedLocalRef sandhook_class(env, FindClassFromCurrentLoader(env, kSandHookClassName));
//            ScopedLocalRef nevercall_class(env,
//                                           FindClassFromCurrentLoader(env,
//                                                                      kSandHookNeverCallClassName));
//            if (sandhook_class == nullptr || nevercall_class == nullptr) { // fail-fast
//                return;
//            }
//            if (!JNI_Load_Ex(env, sandhook_class.get(), nevercall_class.get())) {
//                LOGE("SandHook: HookEntry class error. %d", getpid());
//            }
//
//        }
    }

    jclass
    Context::FindClassFromLoader(JNIEnv *env, jobject class_loader, std::string_view class_name) {
        if (class_loader == nullptr) return nullptr;
        static jclass clz = JNI_FindClass(env, "dalvik/system/DexClassLoader");
        static jmethodID mid = JNI_GetMethodID(env, clz, "loadClass",
                                        "(Ljava/lang/String;)Ljava/lang/Class;");
        jclass ret = nullptr;
        if (!mid) {
            mid = JNI_GetMethodID(env, clz, "findClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        }
        if (LIKELY(mid)) {
            jobject target = JNI_CallObjectMethod(env, class_loader, mid,
                                                  env->NewStringUTF(class_name.data()));
            if (target) {
                return (jclass) target;
            }
        } else {
            LOGE("No loadClass/findClass method found");
        }
        LOGE("Class %s not found", class_name.data());
        return ret;
    }

    inline void Context::FindAndCall(JNIEnv *env, const char *method_name,
                                     const char *method_sig, ...) const {
        if (UNLIKELY(!entry_class_)) {
            LOGE("cannot call method %s, entry class is null", method_name);
            return;
        }
        jmethodID mid = JNI_GetStaticMethodID(env, entry_class_, method_name, method_sig);
        if (LIKELY(mid)) {
            va_list args;
            va_start(args, method_sig);
            env->CallStaticVoidMethodV(entry_class_, mid, args);
            va_end(args);
        } else {
            LOGE("method %s id is null", method_name);
        }
    }

    void
    Context::OnNativeForkSystemServerPre([[maybe_unused]] JNIEnv *env,
                                         [[maybe_unused]] jclass clazz,
                                         [[maybe_unused]]  uid_t uid,
                                         [[maybe_unused]] gid_t gid,
                                         [[maybe_unused]] jintArray gids,
                                         [[maybe_unused]] jint runtime_flags,
                                         [[maybe_unused]] jobjectArray rlimits,
                                         [[maybe_unused]] jlong permitted_capabilities,
                                         [[maybe_unused]] jlong effective_capabilities) {
        Service::instance()->InitService(env);
        PreLoadDex(ConfigManager::GetInjectDexPath());
        skip_ = false;
    }

    void
    Context::OnNativeForkSystemServerPost(JNIEnv *env, jint res) {
        if (res != 0) return;
        auto binder = Service::instance()->RequestBinder(env);
        if (binder) {
            if (void *buf = mmap(nullptr, 1, PROT_READ | PROT_WRITE | PROT_EXEC,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1,
                                 0);
                    buf == MAP_FAILED) {
                 skip_ = true;
                LOGE("skip injecting into android because sepolicy was not loaded properly");
            } else {
                munmap(buf, 1);
            }
        }
        LoadDex(env);
        if (!skip_ && binder) {
            InstallInlineHooks();
            Init(env);
            FindAndCall(env, "forkSystemServerPost", "(Landroid/os/IBinder;)V", binder);
        }
        Service::instance()->HookBridge(*this, env);
    }

    void Context::OnNativeForkAndSpecializePre(JNIEnv *env, jclass clazz,
                                               jint uid, jint gid,
                                               jintArray gids,
                                               jint runtime_flags,
                                               jobjectArray rlimits,
                                               jint mount_external,
                                               jstring se_info,
                                               jstring nice_name,
                                               jintArray fds_to_close,
                                               jintArray fds_to_ignore,
                                               jboolean is_child_zygote,
                                               jstring instruction_set,
                                               jstring app_data_dir) {
        PreLoadDex(ConfigManager::GetInjectDexPath());
        Service::instance()->InitService(env);
        this->uid = uid;
        const auto app_id = uid % PER_USER_RANGE;
        skip_ = false;
        if (!skip_ && !app_data_dir) {
            LOGD("skip injecting into %d because it has no data dir", uid);
            skip_ = true;
        }
        if (!skip_ && is_child_zygote) {
            skip_ = true;
            LOGD("skip injecting into %d because it's a child zygote", uid);
        }

        if (!skip_ && ((app_id >= FIRST_ISOLATED_UID && app_id <= LAST_ISOLATED_UID) ||
                      (app_id >= FIRST_APP_ZYGOTE_ISOLATED_UID &&
                       app_id <= LAST_APP_ZYGOTE_ISOLATED_UID) ||
                      app_id == SHARED_RELRO_UID)) {
            skip_ = true;
            LOGI("skip injecting into %d because it's isolated", uid);
        }
    }

    void
    Context::OnNativeForkAndSpecializePost(JNIEnv *env) {
        const JUTFString process_name(env, nice_name_);
        auto binder = skip_? nullptr : Service::instance()->RequestBinder(env);
        if (binder) {
            LoadDex(env);
            InstallInlineHooks();
            Init(env);
            LOGD("Done prepare");
            // TODO: cache this method
            FindAndCall(env, "forkAndSpecializePost",
                        "(Ljava/lang/String;Ljava/lang/String;Landroid/os/IBinder;)V",
                        app_data_dir_, nice_name_,
                        binder);
            LOGD("injected xposed into %s", process_name.get());
        } else {
            auto context = Context::ReleaseInstance();
            auto service = Service::ReleaseInstance();
            LOGD("skipped %s", process_name.get());
        }
    }
}

#pragma clang diagnostic pop