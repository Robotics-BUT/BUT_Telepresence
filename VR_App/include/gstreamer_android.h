#ifndef GSTREAMER_ANDROID_H
#define GSTREAMER_ANDROID_H

#include <jni.h>
#include <gst/gst.h>

G_BEGIN_DECLS

/* Initialize GStreamer for Android and cache Context/ClassLoader.
   Call this once from your main thread with the Activity (or app) context. */
void     gst_android_init (JNIEnv *env, jclass klass, jobject context);

/* Accessors cached during init. */
jobject  gst_android_get_application_context       (void);
jobject  gst_android_get_application_class_loader  (void);
JavaVM * gst_android_get_java_vm                   (void);
void     gst_android_set_java_vm                   (JavaVM* vm);

/* Ensure the current native thread has the app’s ClassLoader as its
   context ClassLoader. Call this on any thread before touching code
   that reflects into Java (e.g., GStreamer’s androidmedia).
   Returns TRUE on success. */
gboolean gst_set_thread_context_classloader        (void);

G_END_DECLS

#endif /* GSTREAMER_ANDROID_H */