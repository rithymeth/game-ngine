#include <jni.h>

extern "C" JNIEXPORT jstring JNICALL
Java_com_aether_engine_MainActivity_nativeEngineStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF("Aether Engine native core loaded (arm64-v8a)");
}
