package dev.libertycraft.link;

import static dev.libertycraft.link.Proto.MAPPING_BYTES;
import static dev.libertycraft.link.Proto.MAPPING_NAME;
import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import dev.libertycraft.LibertyCraft;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;

/**
 * Native Windows transport (SkyCraft's original): the host creates the named section
 * {@link Proto#MAPPING_NAME}, we open it with kernel32 through the FFM API. Clocks are
 * GetTickCount64 and QueryPerformanceCounter, exactly what the host reads.
 *
 * <p>Only loaded on Windows: looking kernel32 up anywhere else would fail.
 */
final class WinLink extends Link {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;

	private final MethodHandle openFileMapping;
	// OpenFileMappingW's GetLastError, captured right after the call (the JVM may change it later).
	private final StructLayout callState = Linker.Option.captureStateLayout();
	private final VarHandle lastError = this.callState.varHandle(MemoryLayout.PathElement.groupElement("GetLastError"));
	private final MemorySegment openState = Arena.global().allocate(this.callState);
	private int lastOpenError = -1;
	private final MethodHandle mapViewOfFile;
	private final MethodHandle getTickCount64;
	private final MethodHandle queryPerformanceCounter;
	private final MethodHandle queryPerformanceFrequency;
	private final MethodHandle createMutex;
	private MemorySegment runningMutex;
	private final MemorySegment qpcOut = Arena.global().allocate(JAVA_LONG);

	WinLink() {
		Linker linker = Linker.nativeLinker();
		SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		this.openFileMapping = linker.downcallHandle(
			k32.find("OpenFileMappingW").orElseThrow(), FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS), Linker.Option.captureCallState("GetLastError")
		);
		this.mapViewOfFile = linker.downcallHandle(
			k32.find("MapViewOfFile").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG)
		);
		this.getTickCount64 = linker.downcallHandle(k32.find("GetTickCount64").orElseThrow(), FunctionDescriptor.of(JAVA_LONG));
		this.queryPerformanceCounter = linker.downcallHandle(k32.find("QueryPerformanceCounter").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
		this.queryPerformanceFrequency = linker.downcallHandle(k32.find("QueryPerformanceFrequency").orElseThrow(), FunctionDescriptor.of(JAVA_INT, ADDRESS));
		this.createMutex = linker.downcallHandle(k32.find("CreateMutexW").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
	}

	/**
	 * Holds a named mutex ("<link name>_minecraft") for as long as this Minecraft runs, so GTA IV's
	 * ASI plugin knows not to start another one (even before the two have linked up).
	 */
	@Override
	void announce() {
		if (this.runningMutex != null) {
			return;
		}
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment name = arena.allocateFrom(MAPPING_NAME + "_minecraft", StandardCharsets.UTF_16LE);
			this.runningMutex = (MemorySegment) this.createMutex.invokeExact(MemorySegment.NULL, 0, name);
		} catch (Throwable t) {
			LibertyCraft.LOG.warn("[LibertyCraft] couldn't create the running-Minecraft mutex", t);
		}
	}

	@Override
	MemorySegment map() {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment name = arena.allocateFrom(MAPPING_NAME, StandardCharsets.UTF_16LE);
			MemorySegment handle = (MemorySegment) this.openFileMapping.invokeExact(this.openState, FILE_MAP_ALL_ACCESS, 0, name);
			if (handle.address() == 0) {
				// Say why, once per reason: 2 is "GTA IV hasn't made it yet" (normal while it loads),
				// 5 is "not allowed" (GTA IV running as administrator).
				int error = (int) this.lastError.get(this.openState, 0L);
				if (error != this.lastOpenError) {
					this.lastOpenError = error;
					LibertyCraft.LOG.info("[LibertyCraft] can't open GTA IV's shared memory yet (Windows error {}{})", error,
						error == 2 ? ": GTA IV hasn't created it yet" : error == 5 ? ": access denied; is GTA IV running as administrator?" : "");
				}
				return null;
			}
			MemorySegment view = (MemorySegment) this.mapViewOfFile.invokeExact(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
			if (view.address() == 0) {
				LibertyCraft.LOG.error("[LibertyCraft] MapViewOfFile failed");
				return null;
			}
			return view.reinterpret(MAPPING_BYTES);
		} catch (Throwable t) {
			throw new IllegalStateException("opening " + MAPPING_NAME, t);
		}
	}

	@Override
	long nowMs() {
		try {
			return (long) this.getTickCount64.invokeExact();
		} catch (Throwable t) {
			throw new IllegalStateException(t);
		}
	}

	@Override
	synchronized long qpcNow() {
		try {
			int ok = (int) this.queryPerformanceCounter.invokeExact(this.qpcOut);
			return this.qpcOut.get(JAVA_LONG, 0);
		} catch (Throwable t) {
			throw new IllegalStateException(t);
		}
	}

	@Override
	synchronized long qpcFrequency0() {
		try {
			int ok = (int) this.queryPerformanceFrequency.invokeExact(this.qpcOut);
			return this.qpcOut.get(JAVA_LONG, 0);
		} catch (Throwable t) {
			throw new IllegalStateException(t);
		}
	}

	@Override
	boolean hostAlive(int hostPid) {
		// On Windows the header's pid is a real process id, which outlives long loading stalls.
		return ProcessHandle.of(hostPid).map(ProcessHandle::isAlive).orElse(false);
	}
}
