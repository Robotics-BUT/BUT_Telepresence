/**
 * gstreamer_android.c - Android-specific GStreamer initialization
 *
 * Handles JNI_OnLoad, GStreamer init, static plugin registration, GLib/GStreamer
 * log redirection to Android logcat, and DexClassLoader setup for hardware
 * codec access (amcviddec). Called once from main.cpp at app startup.
 */
#include <jni.h>
#include <gst/gst.h>
#include <gio/gio.h>
#include <android/log.h>
#include "log.h"

static jobject _context;
static jobject _class_loader;   // will hold our DexClassLoader
static JavaVM *_java_vm;

static GstClockTime _priv_gst_info_start_time;

#define GST_G_IO_MODULE_DECLARE(name) \
extern void G_PASTE(g_io_module_, G_PASTE(name, _load_static)) (void)

#define GST_G_IO_MODULE_LOAD(name) \
G_PASTE(g_io_module_, G_PASTE(name, _load_static)) ()

/* Declaration of static plugins */
GST_PLUGIN_STATIC_DECLARE(coreelements);
GST_PLUGIN_STATIC_DECLARE(typefindfunctions);
GST_PLUGIN_STATIC_DECLARE(autodetect);
GST_PLUGIN_STATIC_DECLARE(playback);
GST_PLUGIN_STATIC_DECLARE(rtpmanager);
GST_PLUGIN_STATIC_DECLARE(srtp);
GST_PLUGIN_STATIC_DECLARE(compositor);
GST_PLUGIN_STATIC_DECLARE(rtp);
GST_PLUGIN_STATIC_DECLARE(videoconvertscale);
GST_PLUGIN_STATIC_DECLARE(videofilter);
GST_PLUGIN_STATIC_DECLARE(vpx);
GST_PLUGIN_STATIC_DECLARE(androidmedia);
GST_PLUGIN_STATIC_DECLARE(udp);
GST_PLUGIN_STATIC_DECLARE(videoparsersbad);
GST_PLUGIN_STATIC_DECLARE(app);
GST_PLUGIN_STATIC_DECLARE(jpeg);
GST_PLUGIN_STATIC_DECLARE(jpegformat);
GST_PLUGIN_STATIC_DECLARE(libav);
GST_PLUGIN_STATIC_DECLARE(opengl);

/* ---------- Helpers to build & install a DexClassLoader ---------- */

static jobject make_dex_classloader(JNIEnv *env, jobject context) {
    // context.getApplicationInfo()
    jclass ctxCls = (*env)->GetObjectClass(env, context);
    if (!ctxCls) return NULL;

    jmethodID getAppInfo = (*env)->GetMethodID(env, ctxCls, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    if (!getAppInfo) { (*env)->DeleteLocalRef(env, ctxCls); return NULL; }

    jobject appInfo = (*env)->CallObjectMethod(env, context, getAppInfo);
    if ((*env)->ExceptionCheck(env) || !appInfo) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, ctxCls);
        return NULL;
    }

    jclass appInfoCls = (*env)->GetObjectClass(env, appInfo);
    jfieldID sourceDirField = (*env)->GetFieldID(env, appInfoCls, "sourceDir", "Ljava/lang/String;");       // base.apk
    jfieldID dataDirField   = (*env)->GetFieldID(env, appInfoCls, "dataDir",   "Ljava/lang/String;");
    jfieldID nativeLibDirField = (*env)->GetFieldID(env, appInfoCls, "nativeLibraryDir", "Ljava/lang/String;");

    jstring sourceDir = (sourceDirField) ? (jstring)(*env)->GetObjectField(env, appInfo, sourceDirField) : NULL;
    jstring dataDir   = (dataDirField)   ? (jstring)(*env)->GetObjectField(env, appInfo, dataDirField)     : NULL;
    jstring nativeDir = (nativeLibDirField) ? (jstring)(*env)->GetObjectField(env, appInfo, nativeLibDirField) : NULL;

    // parent = context.getClassLoader()
    jmethodID getCL = (*env)->GetMethodID(env, ctxCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject parentCL = getCL ? (*env)->CallObjectMethod(env, context, getCL) : NULL;

    jclass dexCLCls = (*env)->FindClass(env, "dalvik/system/DexClassLoader");
    jmethodID ctor  = dexCLCls ? (*env)->GetMethodID(env, dexCLCls, "<init>",
                                                     "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V") : NULL;

    jobject dexCL = NULL;
    if (dexCLCls && ctor && sourceDir && dataDir) {
        // optimizedDirectory can be app dataDir; Android may redirect to code-cache internally.
        dexCL = (*env)->NewObject(env, dexCLCls, ctor, sourceDir, dataDir, nativeDir, parentCL);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            dexCL = NULL;
        }
    }

    if (dexCL) {
        LOG_INFO("GStreamer: DexClassLoader created from base.apk");
    } else {
        LOG_ERROR("GStreamer: Failed to create DexClassLoader");
    }

    // cleanup locals
    if (dexCLCls)    (*env)->DeleteLocalRef(env, dexCLCls);
    if (parentCL)    (*env)->DeleteLocalRef(env, parentCL);
    if (nativeDir)   (*env)->DeleteLocalRef(env, nativeDir);
    if (dataDir)     (*env)->DeleteLocalRef(env, dataDir);
    if (sourceDir)   (*env)->DeleteLocalRef(env, sourceDir);
    if (appInfoCls)  (*env)->DeleteLocalRef(env, appInfoCls);
    if (appInfo)     (*env)->DeleteLocalRef(env, appInfo);
    (*env)->DeleteLocalRef(env, ctxCls);
    return dexCL;
}

static void install_app_dex_loader(JNIEnv *env) {
    if (!_context) return;
    jobject dexCL = make_dex_classloader(env, _context);
    if (!dexCL) return;

    if (_class_loader) {
        (*env)->DeleteGlobalRef(env, _class_loader);
        _class_loader = NULL;
    }
    _class_loader = (*env)->NewGlobalRef(env, dexCL);
    (*env)->DeleteLocalRef(env, dexCL);

    LOG_INFO("GStreamer: Installed app DexClassLoader as global loader %p", _class_loader);
}

/* Set this thread’s context ClassLoader to our DexClassLoader */
gboolean gst_set_thread_context_classloader(void) {
    if (!_java_vm || !_class_loader) {
        LOG_ERROR("GStreamer: cannot set context ClassLoader (vm=%p, loader=%p)", _java_vm, _class_loader);
        return FALSE;
    }
    JNIEnv* env = NULL;
    if ((*_java_vm)->GetEnv(_java_vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*_java_vm)->AttachCurrentThread(_java_vm, &env, NULL) != 0 || !env) {
            LOG_ERROR("GStreamer: AttachCurrentThread failed");
            return FALSE;
        }
    }

    jclass Thread = (*env)->FindClass(env, "java/lang/Thread");
    if (!Thread) { (*env)->ExceptionClear(env); return FALSE; }
    jmethodID cur = (*env)->GetStaticMethodID(env, Thread, "currentThread", "()Ljava/lang/Thread;");
    jmethodID set = (*env)->GetMethodID(env, Thread, "setContextClassLoader", "(Ljava/lang/ClassLoader;)V");
    jmethodID get = (*env)->GetMethodID(env, Thread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    if (!cur || !set || !get) { (*env)->DeleteLocalRef(env, Thread); return FALSE; }

    jobject thr = (*env)->CallStaticObjectMethod(env, Thread, cur);
    (*env)->CallVoidMethod(env, thr, set, _class_loader);

    // optional sanity check
    jobject got = (*env)->CallObjectMethod(env, thr, get);
    jboolean same = (got && (*env)->IsSameObject(env, got, _class_loader)) ? JNI_TRUE : JNI_FALSE;

    (*env)->DeleteLocalRef(env, got);
    (*env)->DeleteLocalRef(env, thr);
    (*env)->DeleteLocalRef(env, Thread);

    LOG_INFO("GStreamer: set context ClassLoader %s", same ? "OK" : "FAILED");
    return same == JNI_TRUE;
}

/* ---------- Original functions ---------- */

void
gst_android_register_static_plugins(void) {
    GST_PLUGIN_STATIC_REGISTER(coreelements);
    GST_PLUGIN_STATIC_REGISTER(typefindfunctions);

    GST_PLUGIN_STATIC_REGISTER(autodetect);
    GST_PLUGIN_STATIC_REGISTER(playback);
    GST_PLUGIN_STATIC_REGISTER(rtpmanager);
    GST_PLUGIN_STATIC_REGISTER(srtp);
    GST_PLUGIN_STATIC_REGISTER(compositor);
    GST_PLUGIN_STATIC_REGISTER(rtp);
    GST_PLUGIN_STATIC_REGISTER(videoconvertscale);
    GST_PLUGIN_STATIC_REGISTER(videofilter);
    GST_PLUGIN_STATIC_REGISTER(vpx);
    GST_PLUGIN_STATIC_REGISTER(androidmedia);
    GST_PLUGIN_STATIC_REGISTER(udp);
    GST_PLUGIN_STATIC_REGISTER(videoparsersbad);
    GST_PLUGIN_STATIC_REGISTER(app);
    GST_PLUGIN_STATIC_REGISTER(jpeg);
    GST_PLUGIN_STATIC_REGISTER(jpegformat);
    GST_PLUGIN_STATIC_REGISTER(libav);
    GST_PLUGIN_STATIC_REGISTER(opengl);
}

void
gst_android_load_gio_modules(void) {
    GST_G_IO_MODULE_DECLARE(androidmedia);
}

void
glib_print_handler(const gchar *string) {
    LOG_INFO("GStreamer: Glib+stdout %s", string);
}

void
glib_printerr_handler(const gchar *string) {
    LOG_ERROR("Gstreamer: Glib+stderr %s", string);
}

#define CHAR_IS_SAFE(wc) (!((wc < 0x20 && wc != '\t' && wc != '\n' && wc != '\r') || (wc == 0x7f) || (wc >= 0x80 && wc < 0xa0)))
#define FORMAT_UNSIGNED_BUFSIZE ((GLIB_SIZEOF_LONG * 3) + 3)
#define STRING_BUFFER_SIZE (FORMAT_UNSIGNED_BUFSIZE + 32)
#define ALERT_LEVELS (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)
#define DEFAULT_LEVELS (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE)
#define INFO_LEVELS (G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG)

static void
escape_string(GString *string) {
    const char *p = string->str;
    gunichar wc;

    while (p < string->str + string->len) {
        gboolean safe;

        wc = g_utf8_get_char_validated(p, -1);
        if (wc == (gunichar) -1 || wc == (gunichar) -2) {
            gchar *tmp;
            guint pos;

            pos = p - string->str;

            tmp = g_strdup_printf("\\x%02x", (guint) (guchar) *p);
            g_string_erase(string, pos, 1);
            g_string_insert(string, pos, tmp);

            p = string->str + (pos + 4);

            g_free(tmp);
            continue;
        }
        if (wc == '\r') {
            safe = *(p + 1) == '\n';
        } else {
            safe = CHAR_IS_SAFE (wc);
        }

        if (!safe) {
            gchar *tmp;
            guint pos;

            pos = p - string->str;

            tmp = g_strdup_printf("\\u%04x", wc);
            g_string_erase(string, pos, g_utf8_next_char (p) - p);
            g_string_insert(string, pos, tmp);
            g_free(tmp);

            p = string->str + (pos + 6);
        } else
            p = g_utf8_next_char (p);
    }
}

void
glib_log_handler(const gchar *log_domain, GLogLevelFlags log_level, const gchar *message, gpointer user_data) {
    gchar *string;
    GString *gstring;
    const gchar *domains;
    gint android_log_level;
    gchar *tag;

    if ((log_level & DEFAULT_LEVELS) || (log_level >> G_LOG_LEVEL_USER_SHIFT))
        goto emit;

    domains = g_getenv("G_MESSAGES_DEBUG");
    if (((log_level & INFO_LEVELS) == 0) ||
        domains == NULL ||
        (strcmp(domains, "all") != 0 && (!log_domain
                                         || !strstr(domains, log_domain))))
        return;

    emit:

    if (log_domain)
        tag = g_strdup_printf("GLib+%s", log_domain);
    else
        tag = g_strdup("GLib");

    switch (log_level & G_LOG_LEVEL_MASK) {
        case G_LOG_LEVEL_ERROR:
            android_log_level = ANDROID_LOG_ERROR; break;
        case G_LOG_LEVEL_CRITICAL:
            android_log_level = ANDROID_LOG_ERROR; break;
        case G_LOG_LEVEL_WARNING:
            android_log_level = ANDROID_LOG_WARN; break;
        case G_LOG_LEVEL_MESSAGE:
            android_log_level = ANDROID_LOG_INFO; break;
        case G_LOG_LEVEL_INFO:
            android_log_level = ANDROID_LOG_INFO; break;
        case G_LOG_LEVEL_DEBUG:
            android_log_level = ANDROID_LOG_DEBUG; break;
        default:
            android_log_level = ANDROID_LOG_INFO; break;
    }

    gstring = g_string_new(NULL);
    if (!message) {
        g_string_append(gstring, "(NULL) message");
    } else {
        GString *msg = g_string_new(message);
        escape_string(msg);
        g_string_append(gstring, msg->str);
        g_string_free(msg, TRUE);
    }
    string = g_string_free(gstring, FALSE);

    __android_log_print(android_log_level, tag, "%s", string);

    g_free(string);
    g_free(tag);
}

void
gst_debug_logcat(GstDebugCategory *category, GstDebugLevel level,
                 const gchar *file, const gchar *function, gint line,
                 GObject *object, GstDebugMessage *message, gpointer unused) {
    GstClockTime elapsed;
    gint android_log_level;
    gchar *tag;

    if (level > gst_debug_category_get_threshold(category))
        return;

    elapsed = GST_CLOCK_DIFF (_priv_gst_info_start_time, gst_util_get_timestamp());

    switch (level) {
        case GST_LEVEL_ERROR:   android_log_level = ANDROID_LOG_ERROR; break;
        case GST_LEVEL_WARNING: android_log_level = ANDROID_LOG_WARN; break;
        case GST_LEVEL_INFO:    android_log_level = ANDROID_LOG_INFO; break;
        case GST_LEVEL_DEBUG:   android_log_level = ANDROID_LOG_DEBUG; break;
        default:                android_log_level = ANDROID_LOG_VERBOSE; break;
    }

    tag = g_strdup_printf("GStreamer+%s", gst_debug_category_get_name(category));

    if (object) {
        gchar *obj;
        if (GST_IS_PAD (object) && GST_OBJECT_NAME (object)) {
            obj = g_strdup_printf("<%s:%s>", GST_DEBUG_PAD_NAME (object));
        } else if (GST_IS_OBJECT (object) && GST_OBJECT_NAME (object)) {
            obj = g_strdup_printf("<%s>", GST_OBJECT_NAME (object));
        } else if (G_IS_OBJECT (object)) {
            obj = g_strdup_printf("<%s@%p>", G_OBJECT_TYPE_NAME (object), object);
        } else {
            obj = g_strdup_printf("<%p>", object);
        }

        __android_log_print(android_log_level, tag,
                            "%" GST_TIME_FORMAT " %p %s:%d:%s:%s %s\n",
                            GST_TIME_ARGS (elapsed), g_thread_self(),
                            file, line, function, obj, gst_debug_message_get(message));
        g_free(obj);
    } else {
        __android_log_print(android_log_level, tag,
                            "%" GST_TIME_FORMAT " %p %s:%d:%s %s\n",
                            GST_TIME_ARGS (elapsed), g_thread_self(),
                            file, line, function, gst_debug_message_get(message));
    }
    g_free(tag);
}

/* ----- Existing helpers you already had ----- */

static gboolean get_application_dirs(JNIEnv *env, jobject context, gchar **cache_dir, gchar **files_dir) {
    jclass context_class;
    jmethodID get_cache_dir_id, get_files_dir_id;
    jclass file_class;
    jmethodID get_absolute_path_id;
    jobject dir;
    jstring abs_path;
    const gchar *abs_path_str;

    *cache_dir = *files_dir = NULL;

    context_class = (*env)->GetObjectClass(env, context);
    if (!context_class) {
        return FALSE;
    }
    get_cache_dir_id =
            (*env)->GetMethodID(env, context_class, "getCacheDir", "()Ljava/io/File;");
    get_files_dir_id =
            (*env)->GetMethodID(env, context_class, "getFilesDir", "()Ljava/io/File;");
    if (!get_cache_dir_id || !get_files_dir_id) {
        (*env)->DeleteLocalRef(env, context_class);
        return FALSE;
    }

    file_class = (*env)->FindClass(env, "java/io/File");
    if (!file_class) {
        (*env)->DeleteLocalRef(env, context_class);
        return FALSE;
    }
    get_absolute_path_id =
            (*env)->GetMethodID(env, file_class, "getAbsolutePath", "()Ljava/lang/String;");
    if (!get_absolute_path_id) {
        (*env)->DeleteLocalRef(env, context_class);
        (*env)->DeleteLocalRef(env, file_class);
        return FALSE;
    }

    dir = (*env)->CallObjectMethod(env, context, get_cache_dir_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, context_class);
        (*env)->DeleteLocalRef(env, file_class);
        return FALSE;
    }

    if (dir) {
        abs_path = (*env)->CallObjectMethod(env, dir, get_absolute_path_id);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, dir);
            (*env)->DeleteLocalRef(env, context_class);
            (*env)->DeleteLocalRef(env, file_class);
            return FALSE;
        }
        abs_path_str = (*env)->GetStringUTFChars(env, abs_path, NULL);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, abs_path);
            (*env)->DeleteLocalRef(env, dir);
            (*env)->DeleteLocalRef(env, context_class);
            (*env)->DeleteLocalRef(env, file_class);
            return FALSE;
        }
        *cache_dir = abs_path ? g_strdup(abs_path_str) : NULL;

        (*env)->ReleaseStringUTFChars(env, abs_path, abs_path_str);
        (*env)->DeleteLocalRef(env, abs_path);
        (*env)->DeleteLocalRef(env, dir);
    }

    dir = (*env)->CallObjectMethod(env, context, get_files_dir_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, context_class);
        (*env)->DeleteLocalRef(env, file_class);
        return FALSE;
    }
    if (dir) {
        abs_path = (*env)->CallObjectMethod(env, dir, get_absolute_path_id);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, dir);
            (*env)->DeleteLocalRef(env, context_class);
            (*env)->DeleteLocalRef(env, file_class);
            return FALSE;
        }
        abs_path_str = (*env)->GetStringUTFChars(env, abs_path, NULL);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            (*env)->DeleteLocalRef(env, abs_path);
            (*env)->DeleteLocalRef(env, dir);
            (*env)->DeleteLocalRef(env, context_class);
            (*env)->DeleteLocalRef(env, file_class);
            return FALSE;
        }
        *files_dir = files_dir ? g_strdup(abs_path_str) : NULL;

        (*env)->ReleaseStringUTFChars(env, abs_path, abs_path_str);
        (*env)->DeleteLocalRef(env, abs_path);
        (*env)->DeleteLocalRef(env, dir);
    }

    (*env)->DeleteLocalRef(env, file_class);
    (*env)->DeleteLocalRef(env, context_class);

    return TRUE;
}

/* ----- Public getters ----- */
jobject gst_android_get_application_context(void) {
    LOG_INFO("GStreamer: get application context %p", _context);
    return _context;
}
jobject gst_android_get_application_class_loader(void) {
    LOG_INFO("GStreamer: get application class loader %p", _class_loader);
    return _class_loader;
}
JavaVM * gst_android_get_java_vm(void) {
    LOG_INFO("GStreamer: get_java_vm %p", _java_vm);
    return _java_vm;
}

void gst_android_set_java_vm(JavaVM* vm) {
    LOG_INFO("GStreamer: set_java_vm %p", vm);
    _java_vm = vm;
}

/* ----- init paths & install class loader ----- */

static gboolean
init(JNIEnv *env, jobject context) {
    jclass context_cls = NULL;
    jmethodID get_class_loader_id = 0;

    jobject class_loader = NULL;

    context_cls = (*env)->GetObjectClass(env, context);
    if (!context_cls) {
        return FALSE;
    }

    get_class_loader_id = (*env)->GetMethodID(env, context_cls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return FALSE;
    }

    class_loader = (*env)->CallObjectMethod(env, context, get_class_loader_id);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return FALSE;
    }

    if (_context) {
        (*env)->DeleteGlobalRef(env, _context);
    }
    _context = (*env)->NewGlobalRef(env, context);

    // Keep the original as a fallback while we install our DexClassLoader below
    if (_class_loader) {
        (*env)->DeleteGlobalRef(env, _class_loader);
    }
    _class_loader = (*env)->NewGlobalRef(env, class_loader);

    (*env)->DeleteLocalRef(env, class_loader);
    (*env)->DeleteLocalRef(env, context_cls);

    // Replace with DexClassLoader (built from base.apk) and set thread CL now
    install_app_dex_loader(env);
    gst_set_thread_context_classloader();

    return TRUE;
}

void
gst_android_init(JNIEnv *env, jclass klass, jobject context) {
    gchar *cache_dir;
    gchar *files_dir;
    gchar *registry;
    GError *error = NULL;

    if (!init(env, context)) {
        LOG_INFO("GStreamer failed to initialize");
    }

    if (gst_is_initialized()) {
        LOG_INFO("GStreamer already initialized");
        return;
    }

    if (!get_application_dirs(env, context, &cache_dir, &files_dir)) {
        LOG_INFO("GStreamer failed to get application dirs");
    }

    if (cache_dir) {
        g_setenv("TMP", cache_dir, TRUE);
        g_setenv("TEMP", cache_dir, TRUE);
        g_setenv("TMPDIR", cache_dir, TRUE);
        g_setenv("XDG_RUNTIME_DIR", cache_dir, TRUE);
        g_setenv("XDG_CACHE_HOME", cache_dir, TRUE);
        registry = g_build_filename(cache_dir, "registry.bin", NULL);
        g_setenv("GST_REGISTRY", registry, TRUE);
        g_free(registry);
        g_setenv("GST_REUSE_PLUGIN_SCANNER", "no", TRUE);
    }
    if (files_dir) {
        gchar *fontconfig, *certs;

        g_setenv("HOME", files_dir, TRUE);
        g_setenv("XDG_DATA_DIRS", files_dir, TRUE);
        g_setenv("XDG_CONFIG_DIRS", files_dir, TRUE);
        g_setenv("XDG_CONFIG_HOME", files_dir, TRUE);
        g_setenv("XDG_DATA_HOME", files_dir, TRUE);

        fontconfig = g_build_filename(files_dir, "fontconfig", NULL);
        g_setenv("FONTCONFIG_PATH", fontconfig, TRUE);
        g_free(fontconfig);

        certs = g_build_filename(files_dir, "ssl", "certs", "ca-certificates.crt", NULL);
        g_setenv("CA_CERTIFICATES", certs, TRUE);
        g_free(certs);
    }
    g_free(cache_dir);
    g_free(files_dir);

    /* Set GLib print handlers */
    g_set_print_handler(glib_print_handler);
    g_set_printerr_handler(glib_printerr_handler);
    g_log_set_default_handler(glib_log_handler, NULL);

    /* Debug */
    gst_debug_set_default_threshold(GST_LEVEL_WARNING);
    gst_debug_set_active(FALSE);

    gst_debug_remove_log_function (gst_debug_log_default);
    gst_debug_add_log_function ((GstLogFunction) gst_debug_logcat, NULL, NULL);

    /* get time we started for debugging messages */
    _priv_gst_info_start_time = gst_util_get_timestamp();

    if (!gst_init_check(NULL, NULL, &error)) {
        gchar *message = g_strdup_printf("GStreamer initialization failed: %s", error && error->message ? error->message : "(no message)");
        jclass exception_class = (*env)->FindClass(env, "java/lang/Exception");
        LOG_INFO("GStreamer: %s", message);
        (*env)->ThrowNew(env, exception_class, message);
        g_free(message);
        return;
    }

    //gst_debug_set_threshold_for_name("caps*", GST_LEVEL_TRACE);
    //gst_debug_set_threshold_for_name("decodebin*", GST_LEVEL_TRACE);
    //gst_debug_set_threshold_for_name("identity*", GST_LEVEL_WARNING);

    gst_android_register_static_plugins();
    gst_android_load_gio_modules();

    GstElementFactory *dummy = gst_element_factory_find("amcviddec-omxqcomvideodecoderavc");
    if (dummy) {
        LOG_INFO("Gstreamer: amcviddec-omxqcomvideodecoderavc found");
        gst_object_unref(dummy);
    } else {
        LOG_INFO("Gstreamer: amcviddec-omxqcomvideodecoderavc not found");
    }

    GstPlugin *plugin = gst_registry_find_plugin(gst_registry_get(), "androidmedia");
    if (plugin) {
        LOG_INFO("GStreamer: androidmedia plugin found");
    } else {
        LOG_INFO("GStreamer: androidmedia plugin not found");
    }

    LOG_INFO("GStreamer initialization complete");
}

#if 0
static JNINativeMethod native_methods[] = {
  {"nativeInit", "(Landroid/content/Context;)V", (void *) gst_android_init}
};
#endif

jint
JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env = NULL;

    LOG_INFO("GStreamer: JNI_OnLoad");

    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_4) != JNI_OK) {
        LOG_INFO("GStreamer: Could not retrieve JNIEnv");
        return 0;
    }

    _java_vm = vm;
    return JNI_VERSION_1_4;
}
