(() => {
    'use strict';

    // --- 1. Configuration & Constants ---
    const CONFIG = {
        WIDTH: 240,
        HEIGHT: 64,
        CLOCK_FREQ: 2457600
    };

    const KEY_MAP = {
        'KeyZ': [0, 0], 'KeyX': [0, 1], 'KeyC': [0, 2], 'KeyV': [0, 3],
        'KeyB': [0, 4], 'KeyN': [0, 5], 'KeyM': [0, 6], 'Comma': [0, 7],
        'KeyA': [1, 0], 'KeyS': [1, 1], 'KeyD': [1, 2], 'KeyF': [1, 3],
        'KeyG': [1, 4], 'KeyH': [1, 5], 'KeyJ': [1, 6], 'KeyK': [1, 7],
        'KeyQ': [2, 0], 'KeyW': [2, 1], 'KeyE': [2, 2], 'KeyR': [2, 3],
        'KeyT': [2, 4], 'KeyY': [2, 5], 'KeyU': [2, 6], 'KeyI': [2, 7],
        'KeyO': [3, 0], 'KeyP': [3, 1], 'Quote': [3, 2],
        'BracketLeft': [3, 3], 'Backslash': [3, 4], 'BracketRight': [3, 5],
        'Backquote': [3, 6], 'Slash': [3, 7],
        'Digit1': [4, 0], 'Digit2': [4, 1], 'Digit3': [4, 2], 'Digit4': [4, 3],
        'Digit5': [4, 4], 'Digit6': [4, 5], 'Digit7': [4, 6], 'Digit8': [4, 7],
        'Digit9': [5, 0], 'Digit0': [5, 1], 'Semicolon': [5, 2], 'Minus': [5, 3],
        'Equal': [5, 4], 'Space': [5, 5], 'Insert': [5, 6], 'Period': [5, 7],
        'Backspace': [6, 0], 'ArrowUp': [6, 1], 'ArrowDown': [6, 2], 'ArrowLeft': [6, 3],
        'ArrowRight': [6, 4], 'Tab': [6, 5], 'Escape': [6, 6], 'Enter': [6, 7],
        'F1': [7, 0], 'F2': [7, 1], 'F3': [7, 2], 'F4': [7, 3], 'F5': [7, 4],
        'Pause': [7, 7],
        'ShiftLeft': [8, 0], 'ShiftRight': [8, 0],
        'ControlLeft': [8, 1], 'ControlRight': [8, 1],
        'AltLeft': [8, 2], 'AltRight': [8, 2],
        'CapsLock': [8, 4]
    };

    const SHADERS = {
        VS: `#version 300 es
            in vec2 a_position;
            out vec2 v_texCoord;
            void main() {
                gl_Position = vec4(a_position, 0.0, 1.0);
                v_texCoord = a_position * 0.5 + 0.5;
                v_texCoord.y = 1.0 - v_texCoord.y;
            }`,
        FS: `#version 300 es
            precision mediump float;
            in vec2 v_texCoord;
            out vec4 fragColor;

            uniform sampler2D u_image;
            uniform vec2 u_resolution;
            uniform vec2 u_aaWidth;

            uniform vec3 u_colorBg;
            uniform vec3 u_colorPixel;
            uniform vec3 u_colorShadow;

            const float PIXEL_GAP = 0.1;
            const vec2 SHADOW_OFFSET = vec2(0.20, 0.25);
            const float CONTRAST = 1.00;
            const float BRIGHTNESS = 0.0;

            float getPixelMask(vec2 gridFract, vec2 aaWidth) {
                vec2 bl = smoothstep(vec2(PIXEL_GAP) - aaWidth, vec2(PIXEL_GAP) + aaWidth, gridFract);
                vec2 tr = smoothstep(vec2(PIXEL_GAP) - aaWidth, vec2(PIXEL_GAP) + aaWidth, 1.0 - gridFract);
                return bl.x * bl.y * tr.x * tr.y;
            }

            float hash(vec2 p) {
                return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453123);
            }

            void main() {
                vec2 pixelCoord = v_texCoord * u_resolution;
                vec2 gridId = floor(pixelCoord);
                vec2 gridFract = fract(pixelCoord);

                vec2 texUV = (gridId + 0.5) / u_resolution;
                vec4 texColor = texture(u_image, texUV);
                float pixelActive = step(texColor.r, 0.52);

                vec2 shadowCoord = pixelCoord - SHADOW_OFFSET;
                vec2 shadowGridId = floor(shadowCoord);
                vec2 shadowUV = (shadowGridId + 0.5) / u_resolution;
                vec4 shadowTexColor = texture(u_image, shadowUV);
                float shadowActive = step(shadowTexColor.r, 0.52);

                float pixelMask = getPixelMask(gridFract, u_aaWidth);
                float shadowMask = getPixelMask(fract(shadowCoord), u_aaWidth);

                float lightFactor = dot(v_texCoord, vec2(0.7, -0.7)) * 0.1 + 0.95;
                vec3 baseColor = u_colorBg * lightFactor;
                baseColor += (hash(gl_FragCoord.xy) * 0.08);

                vec3 finalColor = baseColor;
                finalColor = mix(finalColor, u_colorShadow, shadowMask * shadowActive);
                finalColor = mix(finalColor, u_colorPixel, pixelMask * pixelActive);

                finalColor *= vec3(1.0, 0.99, 0.96);
                finalColor = (finalColor - 0.5) * CONTRAST + 0.5 + BRIGHTNESS;
                finalColor = clamp(finalColor, 0.0, 1.0);

                fragColor = vec4(finalColor, 1.0);
            }`
    };

    // --- 2. Main Emulator Class ---
    class ShiftyEmulator {
        constructor() {
            this.vm = null;
            this.debugInfo = null;
            this.sourceMap = new Array(65536).fill(null);
            this.activeBreakpoints = new Set();
            this.tempBreakpoints = new Set();
            this.isPaused = false;
            this.currentActiveLineElement = null;

            this.cpuView = null;
            this.pixelView = null;
            this.bufferLength = CONFIG.WIDTH * CONFIG.HEIGHT * 4;

            this.timeLast = performance.now();
            this.tStatesTooMany = 0;
            this.frameCount = 0;

            // DOM Elements Mapping
            this.ui = {
                canvas: document.getElementById('canvas'),
                cpu: {
                    pc: document.getElementById('cpu-pc'), sp: document.getElementById('cpu-sp'),
                    a: document.getElementById('cpu-a'), b: document.getElementById('cpu-b'),
                    c: document.getElementById('cpu-c'), d: document.getElementById('cpu-d'),
                    e: document.getElementById('cpu-e'), h: document.getElementById('cpu-h'),
                    l: document.getElementById('cpu-l'), bc: document.getElementById('cpu-bc'),
                    de: document.getElementById('cpu-de'), hl: document.getElementById('cpu-hl'),
                    cy: document.getElementById('cpu-cy'), p: document.getElementById('cpu-p'),
                    ac: document.getElementById('cpu-ac'), z: document.getElementById('cpu-z'),
                    s: document.getElementById('cpu-s')
                },
                debug: {
                    fileSelector: document.getElementById('file-selector'),
                    sourceCode: document.getElementById('source-code'),
                    btnRunPause: document.getElementById('btn-run-pause'),
                    btnStepInto: document.getElementById('btn-step-into'),
                    btnStepOver: document.getElementById('btn-step-over'),
                    btnStepOut: document.getElementById('btn-step-out')
                }
            };
        }

        async fetchBytes(path) {
            return await (await fetch(path)).arrayBuffer();
        }

        // --- Core Initialization ---
        async init() {
            if (!this.setupWebGL()) return;

            this.setupWindowListeners();

            try {
                const { instance } = await WebAssembly.instantiateStreaming(await fetch('web_shifty.wasm'));
                this.vm = instance.exports;

                const gameBytes = new Uint8Array(await this.fetchBytes('shifty.co'));
                const gameBufferPtr = this.vm.allocate(gameBytes.byteLength);
                const gameBuffer = new Uint8Array(this.vm.memory.buffer, gameBufferPtr, gameBytes.byteLength);
                gameBuffer.set(gameBytes);
                this.bindWasmMemory();

                await this.loadDebugInfo();
                this.setupDebugger();
                this.setupInputListeners();
                this.vm.reset_emulator_with_co_file(gameBufferPtr, gameBytes.byteLength);
                this.setupEditableFlags();

                requestAnimationFrame(this.renderLoop.bind(this));
            } catch (error) {
                console.error("Failed to initialize Wasm or Emulator:", error);
            }
        }

        bindWasmMemory() {
            this.pixelView = new Uint8Array(this.vm.memory.buffer, this.vm.get_canvas_buffer(), this.bufferLength);
            this.cpuView = new DataView(this.vm.memory.buffer, this.vm.get_cpu_state_ptr(), 16);
            this.sourceMap32 = new Uint32Array(this.vm.memory.buffer, this.vm.get_source_map_ptr(), 65536);
        }

        // --- WebGL Setup ---
        setupWebGL() {
            this.gl = this.ui.canvas.getContext('webgl2');
            if (!this.gl) {
                alert("WebGL2 is required for this advanced LCD shader effect.");
                return false;
            }

            const createShader = (type, source) => {
                const shader = this.gl.createShader(type);
                this.gl.shaderSource(shader, source);
                this.gl.compileShader(shader);
                if (!this.gl.getShaderParameter(shader, this.gl.COMPILE_STATUS)) {
                    console.error(this.gl.getShaderInfoLog(shader));
                    this.gl.deleteShader(shader);
                }
                return shader;
            };

            this.program = this.gl.createProgram();
            this.gl.attachShader(this.program, createShader(this.gl.VERTEX_SHADER, SHADERS.VS));
            this.gl.attachShader(this.program, createShader(this.gl.FRAGMENT_SHADER, SHADERS.FS));
            this.gl.linkProgram(this.program);
            this.gl.useProgram(this.program);

            const positionBuffer = this.gl.createBuffer();
            this.gl.bindBuffer(this.gl.ARRAY_BUFFER, positionBuffer);
            this.gl.bufferData(this.gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1]), this.gl.STATIC_DRAW);

            const posLoc = this.gl.getAttribLocation(this.program, "a_position");
            this.gl.enableVertexAttribArray(posLoc);
            this.gl.vertexAttribPointer(posLoc, 2, this.gl.FLOAT, false, 0, 0);

            this.texture = this.gl.createTexture();
            this.gl.bindTexture(this.gl.TEXTURE_2D, this.texture);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_S, this.gl.CLAMP_TO_EDGE);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_T, this.gl.CLAMP_TO_EDGE);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MIN_FILTER, this.gl.NEAREST);
            this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MAG_FILTER, this.gl.NEAREST);
            this.gl.texImage2D(this.gl.TEXTURE_2D, 0, this.gl.RGBA, CONFIG.WIDTH, CONFIG.HEIGHT, 0, this.gl.RGBA, this.gl.UNSIGNED_BYTE, null);

            this.gl.uniform2f(this.gl.getUniformLocation(this.program, "u_resolution"), CONFIG.WIDTH, CONFIG.HEIGHT);

            this.aaLoc = this.gl.getUniformLocation(this.program, "u_aaWidth");
            this.colorBgLoc = this.gl.getUniformLocation(this.program, "u_colorBg");
            this.colorPixelLoc = this.gl.getUniformLocation(this.program, "u_colorPixel");
            this.colorShadowLoc = this.gl.getUniformLocation(this.program, "u_colorShadow");

            return true;
        }

        setupWindowListeners() {
            const resizeCanvas = () => {
                const dpr = window.devicePixelRatio || 1;
                const displayWidth  = Math.round(this.ui.canvas.clientWidth * dpr);
                const displayHeight = Math.round(this.ui.canvas.clientHeight * dpr);

                if (this.ui.canvas.width !== displayWidth || this.ui.canvas.height !== displayHeight) {
                    this.ui.canvas.width = displayWidth;
                    this.ui.canvas.height = displayHeight;
                    this.gl.viewport(0, 0, this.ui.canvas.width, this.ui.canvas.height);
                    this.gl.uniform2f(this.aaLoc, CONFIG.WIDTH / this.ui.canvas.width, CONFIG.HEIGHT / this.ui.canvas.height);
                }
            };
            window.addEventListener('resize', resizeCanvas);
            resizeCanvas();
        }

        updateShaderColors() {
            this.gl.uniform3fv(this.colorBgLoc, this.getCSSColorAsVec3('--lcd-bg'));
            this.gl.uniform3fv(this.colorPixelLoc, this.getCSSColorAsVec3('--lcd-pixel'));
            this.gl.uniform3fv(this.colorShadowLoc, this.getCSSColorAsVec3('--lcd-shadow'));
        }

        // --- Input Handling ---
        setupEditableFlags() {
            // Map each UI element to its corresponding byte offset in the Wasm memory
            const flagMap = [
                { el: this.ui.cpu.cy, offset: 11 },
                { el: this.ui.cpu.p,  offset: 12 },
                { el: this.ui.cpu.ac, offset: 13 },
                { el: this.ui.cpu.z,  offset: 14 },
                { el: this.ui.cpu.s,  offset: 15 }
            ];

            flagMap.forEach(({ el, offset }) => {
                if (!el) return;

                // Make it obvious that the flag is clickable
                el.style.cursor = 'pointer';

                el.addEventListener('click', () => {
                    if (!this.cpuView) return; // Ensure memory is bound

                    // Read current value, toggle it, and write it back
                    const currentVal = this.cpuView.getUint8(offset);
                    const newVal = currentVal === 0 ? 1 : 0;
                    this.cpuView.setUint8(offset, newVal);

                    // Force an immediate UI update
                    this.updateCpuUI(this.getCpuState());
                });
            });
        }

        setupInputListeners() {
            const handleKey = (e, isPressed) => {
                if (isPressed) {
                    if (e.code == 'F11') {
                        if (this.isPaused) e.preventDefault();
                        if (e.shiftKey) this.stepOut();
                        else            this.stepInto();
                        return;
                    }

                    if (e.code == 'F10') {
                        e.preventDefault();
                        this.stepOver();
                        return;
                    }

                    if (e.code == 'F7') {
                        e.preventDefault();
                        this.setPaused(!this.isPaused);
                        return;
                    }

                    if (e.code == 'F9') {
                        e.preventDefault();
                        this.toggleBreakpoint();
                        return
                    }
                }

                const mappedKey = KEY_MAP[e.code];
                if (mappedKey && this.vm) {
                    // e.preventDefault();
                    this.vm.set_key_state(mappedKey[0], mappedKey[1], isPressed ? 1 : 0);
                }
            };

            const passive = {passive: true};
            const active = {passive: false};
            window.addEventListener('keydown', (e) => handleKey(e, true), active);
            window.addEventListener('keyup', (e) => handleKey(e, false), passive);

            document.querySelectorAll('.touch-btn').forEach(btn => {
                const row = parseInt(btn.dataset.row, 10);
                const col = parseInt(btn.dataset.col, 10);

                const applyState = (e, state) => {
                    e.preventDefault();
                    if (this.vm) this.vm.set_key_state(row, col, state);
                };

                btn.addEventListener('touchstart', (e) => applyState(e, 1), active);
                btn.addEventListener('touchend', (e) => applyState(e, 0), active);
                btn.addEventListener('touchcancel', (e) => applyState(e, 0), active);
                btn.addEventListener('mousedown', (e) => applyState(e, 1));
                btn.addEventListener('mouseup', (e) => applyState(e, 0));
                btn.addEventListener('mouseleave', (e) => applyState(e, 0));
            });
        }

        // --- Debugger & Source Control ---
        async loadDebugInfo() {
            try {
                const response = await fetch('debug.json');
                this.debugInfo = await response.json();

                for (const lineData of this.debugInfo.lines) {
                    for (const addr of lineData.addresses) {
                        this.sourceMap[addr] = { fileId: lineData.file_id, line: lineData.line };
                        this.sourceMap32[addr] = (lineData.line & 0xffff) | ((lineData.file_id & 0xffff) << 16);
                    }
                }

                if (this.ui.debug.fileSelector) {
                    this.debugInfo.files.forEach((file, index) => {
                        const option = document.createElement('option');
                        option.value = index;
                        option.text = file.split('\\').pop();
                        this.ui.debug.fileSelector.appendChild(option);
                    });

                    this.ui.debug.fileSelector.addEventListener('change', (e) => this.renderSourceFile(parseInt(e.target.value)));
                }
                this.renderSourceFile(0);
            } catch (err) {
                console.warn("No debug.json found or failed to load. Debugging disabled.", err);
            }
        }

        renderSourceFile(fileId) {
            if (!this.ui.debug.sourceCode) return;
            this.ui.debug.sourceCode.innerHTML = '';

            const lines = this.debugInfo.file_contents[fileId].split('\n');
            lines.forEach((text, index) => {
                const lineNum = index + 1;
                const lineEl = document.createElement('div');
                lineEl.className = 'source-line';
                lineEl.id = `file-${fileId}-line-${lineNum}`;

                const gutter = document.createElement('div');
                gutter.className = 'gutter';
                gutter.innerText = lineNum;

                const lineData = this.debugInfo.lines.find(l => l.file_id === fileId && l.line === lineNum);
                if (lineData && lineData.addresses.length > 0) {
                    const firstAddr = lineData.addresses[0];
                    if (this.activeBreakpoints.has(firstAddr)) {
                        gutter.classList.add('breakpoint');
                    }
                    lineEl.addEventListener('click', () => this.toggleBreakpoint(fileId, lineNum, lineData.addresses, gutter));
                }

                const code = document.createElement('div');
                code.className = 'code-text';
                code.innerText = text;

                lineEl.appendChild(gutter);
                lineEl.appendChild(code);
                this.ui.debug.sourceCode.appendChild(lineEl);
            });
        }

        toggleBreakpoint(fileId, line, addresses, gutterEl) {
            if (!this.vm) return;

            const firstAddr = addresses[0];
            const isSet = this.activeBreakpoints.has(firstAddr);

            if (isSet) {
                this.activeBreakpoints.delete(firstAddr);
                this.vm.clear_breakpoint(firstAddr);
                gutterEl.classList.remove('breakpoint');
            } else {
                this.activeBreakpoints.add(firstAddr);
                this.vm.set_breakpoint(firstAddr, false);
                gutterEl.classList.add('breakpoint');
            }
        }

        updateDebuggerState(cpu) {
            if (this.currentActiveLineElement) {
                this.currentActiveLineElement.classList.remove('active');
            }

            const mapping = this.sourceMap[cpu.pc];
            if (mapping) {
                if (this.ui.debug.fileSelector && parseInt(this.ui.debug.fileSelector.value) !== mapping.fileId) {
                    this.ui.debug.fileSelector.value = mapping.fileId;
                    this.renderSourceFile(mapping.fileId);
                }

                const lineEl = document.getElementById(`file-${mapping.fileId}-line-${mapping.line}`);
                if (lineEl) {
                    lineEl.classList.add('active');

                    // Changed from 'smooth' to 'auto' to prevent animation cancellations
                    lineEl.scrollIntoView({ block: 'center', behavior: 'auto' });
                    this.currentActiveLineElement = lineEl;
                }
            }
            this.updateCpuUI(cpu);
        }

        setPaused(paused) {
            this.isPaused = paused;
            if (this.ui.debug.btnRunPause) {
                this.ui.debug.btnRunPause.innerText = paused ? "Run" : "Pause";
            }

            if (!this.vm) return;

            this.tempBreakpoints.forEach(addr => {
                if (!this.activeBreakpoints.has(addr)) {
                    this.vm.set_breakpoint(addr, true);
                }
            });
            this.tempBreakpoints.clear();

            if (paused) this.updateDebuggerState(this.getCpuState());
            else this.currentActiveLineElement.classList.remove('active');
        }

        stepInto() {
            if (!this.isPaused) return;
            const startMapping = this.sourceMap[this.getCpuProgramCounter()];

            let safeguards = 0;

            // Step until we reach a DIFFERENT mapped line, or hit safeguard limit
            do {
                this.vm.step(0);
                const currMapping = this.sourceMap[this.getCpuProgramCounter()];
                safeguards++;

                // If we land on a mapped line, check if it's a new line
                if (currMapping) {
                    // If we started unmapped, stop at the first mapped line we find
                    if (!startMapping) break;

                    // If it's a different line/file from where we started, stop
                    if (currMapping.fileId !== startMapping.fileId || currMapping.line !== startMapping.line) {
                        break;
                    }
                }
            } while (safeguards < 10000); // Increased safeguard to allow stepping over larger unmapped blocks

            this.updateDebuggerState(this.getCpuState());
        }

        stepOver() {
            if (!this.isPaused) return;
            const startMapping = this.sourceMap[this.getCpuProgramCounter()];

            let safeguards = 0;

            // Step until we reach a DIFFERENT mapped line, or hit safeguard limit
            do {
                this.vm.step(1);

                const currMapping = this.sourceMap[this.getCpuProgramCounter()];
                safeguards++;

                // If we land on a mapped line, check if it's a new line
                if (currMapping) {
                    // If we started unmapped, stop at the first mapped line we find
                    if (!startMapping) break;

                    // If it's a different line/file from where we started, stop
                    if (currMapping.fileId !== startMapping.fileId || currMapping.line !== startMapping.line) {
                        break;
                    }
                }
            } while (safeguards < 10000); // Increased safeguard to allow stepping over larger unmapped blocks

            this.updateDebuggerState(this.getCpuState());
        }

        stepOut() {
            if (!this.isPaused) return;

            const retAddr = this.vm.set_break_on_return();
            if (retAddr >= 0 && retAddr <= 65535) {
                this.tempBreakpoints.add(retAddr);
            }
            this.setPaused(false);
        }

        setupDebugger() {
            this.ui.debug.btnRunPause?.addEventListener('click', () => {
                if (this.isPaused) {
                    const cpu = this.getCpuState();
                    if (this.activeBreakpoints.has(cpu.pc)) this.vm.step(0);
                    this.setPaused(false);
                } else {
                    this.setPaused(true);
                }
            });

            this.ui.debug.btnStepInto?.addEventListener('click', this.stepInto.bind(this));
            this.ui.debug.btnStepOver?.addEventListener('click', this.stepOver.bind(this));
            this.ui.debug.btnStepOut?.addEventListener('click', this.stepOut.bind(this));
        }

        // --- CPU & UI Utilities ---
        getCSSColorAsVec3(cssVarName) {
            const rootStyles = getComputedStyle(document.documentElement);
            const colorStr = rootStyles.getPropertyValue(cssVarName).trim();
            const match = colorStr.match(/\d+/g);

            if (match && match.length >= 3) {
                return [
                    parseInt(match[0]) / 255.0,
                    parseInt(match[1]) / 255.0,
                    parseInt(match[2]) / 255.0
                ];
            }
            return [0, 0, 0];
        }

        toHex(val, padding) {
            return val.toString(16).padStart(padding, '0').toUpperCase();
        }

        getCpuProgramCounter() {
            return this.cpuView.getUint16(0, true);
        }

        getCpuStackPointer() {
            return this.cpuView.getUint16(2, true);
        }


        getCpuState() {
            const littleEndian = true;
            return {
                pc: this.cpuView.getUint16(0, littleEndian),
                sp: this.cpuView.getUint16(2, littleEndian),
                a:  this.cpuView.getUint8(4),
                b:  this.cpuView.getUint8(5),
                c:  this.cpuView.getUint8(6),
                d:  this.cpuView.getUint8(7),
                e:  this.cpuView.getUint8(8),
                h:  this.cpuView.getUint8(9),
                l:  this.cpuView.getUint8(10),
                cy: this.cpuView.getUint8(11),
                p:  this.cpuView.getUint8(12),
                ac: this.cpuView.getUint8(13),
                z:  this.cpuView.getUint8(14),
                s:  this.cpuView.getUint8(15)
            };
        }

        updateCpuUI(cpu) {
            if (!this.ui.cpu.pc) return; // Fail gracefully if debugger UI doesn't exist

            this.ui.cpu.pc.textContent = this.toHex(cpu.pc, 4);
            this.ui.cpu.sp.textContent = this.toHex(cpu.sp, 4);

            this.ui.cpu.a.textContent = this.toHex(cpu.a, 2);
            this.ui.cpu.b.textContent = this.toHex(cpu.b, 2);
            this.ui.cpu.c.textContent = this.toHex(cpu.c, 2);
            this.ui.cpu.d.textContent = this.toHex(cpu.d, 2);
            this.ui.cpu.e.textContent = this.toHex(cpu.e, 2);
            this.ui.cpu.h.textContent = this.toHex(cpu.h, 2);
            this.ui.cpu.l.textContent = this.toHex(cpu.l, 2);

            this.ui.cpu.bc.textContent = this.toHex((cpu.b << 8) | cpu.c, 4);
            this.ui.cpu.de.textContent = this.toHex((cpu.d << 8) | cpu.e, 4);
            this.ui.cpu.hl.textContent = this.toHex((cpu.h << 8) | cpu.l, 4);

            const updateFlag = (el, val) => {
                if (el) {
                    el.textContent = val;
                    el.classList.toggle('active', val !== 0);
                }
            };

            updateFlag(this.ui.cpu.cy, cpu.cy);
            updateFlag(this.ui.cpu.p, cpu.p);
            updateFlag(this.ui.cpu.ac, cpu.ac);
            updateFlag(this.ui.cpu.z, cpu.z);
            updateFlag(this.ui.cpu.s, cpu.s);
        }

        // --- Render Loop ---
        renderLoop(timeNow) {
            if (!this.isPaused) {
                let deltaTime = Math.min((timeNow - this.timeLast) / 1000.0, 0.1);
                const tStateGoal = Math.max(0, CONFIG.CLOCK_FREQ * deltaTime - this.tStatesTooMany);

                const result = this.vm.run(tStateGoal);

                if (result === -1) {
                    // Breakpoint hit
                    this.tStatesTooMany = 0;
                    this.setPaused(true);
                } else {
                    this.tStatesTooMany = result;
                }
            }

            // CRITICAL: Re-bind views if WebAssembly memory grew
            if (this.pixelView.byteLength === 0) {
                this.bindWasmMemory();
            }

            this.updateShaderColors();
            this.gl.bindTexture(this.gl.TEXTURE_2D, this.texture);
            this.gl.texSubImage2D(this.gl.TEXTURE_2D, 0, 0, 0, CONFIG.WIDTH, CONFIG.HEIGHT, this.gl.RGBA, this.gl.UNSIGNED_BYTE, this.pixelView);
            this.gl.drawArrays(this.gl.TRIANGLES, 0, 6);

            if (!this.isPaused && this.frameCount % 4 === 0) {
                this.updateCpuUI(this.getCpuState());
            }

            this.frameCount++;
            this.timeLast = timeNow;
            requestAnimationFrame(this.renderLoop.bind(this));
        }
    }

    // --- Bootstrap ---
    const emulator = new ShiftyEmulator();
    emulator.init();
    window.emulator = emulator;
})();