#ifdef JKK_GL_LOADED
#error "gl.h already loaded"
#else
#define JKK_GL_LOADED
#endif

#define GL_FUNC_LIST(X) \
    X(PFNGLCREATESHADERPROC,            glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,            glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,           glCompileShader) \
    X(PFNGLGETSHADERIVPROC,             glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,        glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC,           glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,            glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,             glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,            glGetProgramiv) \
    X(PFNGLUSEPROGRAMPROC,              glUseProgram) \
    X(PFNGLGENBUFFERSPROC,              glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,              glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,              glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC,           glBufferSubData) \
    X(PFNGLGENVERTEXARRAYSPROC,         glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,         glBindVertexArray) \
    X(PFNGLGETATTRIBLOCATIONPROC,       glGetAttribLocation) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer) \
    X(PFNGLVERTEXATTRIBDIVISORPROC,     glVertexAttribDivisor) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC,               glUniform1i) \
    X(PFNGLUNIFORM2FPROC,               glUniform2f) \
    X(PFNGLUNIFORM3FPROC,               glUniform3f) \
    X(PFNGLUNIFORM4FPROC,               glUniform4f) \
    X(PFNGLDRAWARRAYSINSTANCEDPROC,     glDrawArraysInstanced) \
    /*end*/

#define X(type, name) type name;
GL_FUNC_LIST(X)
#undef X

bool load_gl_extensions() {
    #define GL_FUNC_LOAD(type, name) \
        name = (type)SDL_GL_GetProcAddress(#name); \
        if (!name) { SDL_Log("Failed to load " #name); return false; }

    GL_FUNC_LIST(GL_FUNC_LOAD)

    return true;
}

GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[512];
        glGetShaderInfoLog(shader, 512, NULL, info);
        SDL_Log("Shader compilation failed: %s", info);
    }
    return shader;
}
