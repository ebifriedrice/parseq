package parseq.sqltree;

/**
 * JNI surface of libsqltree. The native implementations live in
 * native/src/sqltree.c (Java_parseq_sqltree_Sqltree_*) (symbols Java_parseq_native_Sqltree_*).
 *
 * The library must be on java.library.path; loading is triggered by
 * this class so callers only depend on plain Java.
 */
public final class Sqltree {
  static {
    System.loadLibrary("sqltree");
  }

  private Sqltree() {}

  public static native String version();

  /** Open a session; returns an opaque long handle, 0 on failure. */
  public static native long open(String path);

  public static native void close(long handle);

  /** Parse SQL in capture mode; returns EDN text. */
  public static native String parse(long handle, String sql);

  /** Execute DDL to populate the session schema; null or EDN error map. */
  public static native String loadSchema(long handle, String ddl);
}
