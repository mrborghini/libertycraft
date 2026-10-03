package dev.libertycraft.link;

import static dev.libertycraft.link.Proto.BRIDGE_FILE;
import static dev.libertycraft.link.Proto.H_HOST_HEARTBEAT;
import static dev.libertycraft.link.Proto.MAPPING_BYTES;
import static dev.libertycraft.link.Proto.OFF_HEADER;
import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import dev.libertycraft.LibertyCraft;
import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.invoke.MethodHandle;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFilePermissions;

/**
 * Linux transport: Minecraft runs natively, GTA IV under Wine/Proton. Wine has no way to share a
 * named section with a native process, but its CreateFileMapping on a regular file is an
 * mmap(MAP_SHARED) of that file, so both sides map the same tmpfs file
 * ({@link Proto#BRIDGE_FILE}, which GTA IV opens as {@code Z:\dev\shm\libertycraft-bridge}) and see
 * the same pages.
 *
 * <p>Whoever starts first creates the file; we only ever grow it, never shrink or truncate it to
 * zero, because shrinking a file someone else has mapped makes their next access SIGBUS. The host
 * writes the header; until it has, {@link Link#poll()} just keeps checking.
 */
final class PosixLink extends Link {
	// clockid_t CLOCK_MONOTONIC_RAW on Linux (and macOS). Wine's GetTickCount64 and
	// QueryPerformanceCounter are both derived from it, so our stamps are comparable with GTA IV's.
	private static final int CLOCK_MONOTONIC_RAW = 4;
	// Wine reports a fixed 10 MHz QueryPerformanceFrequency: QPC = CLOCK_MONOTONIC_RAW / 100 ns.
	private static final long QPC_FREQUENCY = 10_000_000L;

	private static final StructLayout TIMESPEC = MemoryLayout.structLayout(JAVA_LONG.withName("tv_sec"), JAVA_LONG.withName("tv_nsec"));

	private final MethodHandle clockGettime;
	private final MemorySegment timespec = Arena.global().allocate(TIMESPEC);
	private boolean warnedMissing;

	PosixLink() {
		Linker linker = Linker.nativeLinker();
		this.clockGettime = linker.downcallHandle(
			linker.defaultLookup().find("clock_gettime").orElseThrow(), FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS)
		);
	}

	@Override
	MemorySegment map() {
		Path path = Path.of(BRIDGE_FILE);
		try (FileChannel channel = FileChannel.open(path, StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE)) {
			if (channel.size() < MAPPING_BYTES) {
				// FileChannel.truncate never grows a file; writing its last byte does. In tmpfs the
				// result is sparse: a page costs memory only once someone touches it.
				channel.write(java.nio.ByteBuffer.allocate(1), MAPPING_BYTES - 1);
			}
			try {
				// Only us (and GTA IV, which runs as the same user): the bridge carries keyboard input.
				Files.setPosixFilePermissions(path, PosixFilePermissions.fromString("rw-------"));
			} catch (IOException | UnsupportedOperationException e) {
				LibertyCraft.LOG.warn("[LibertyCraft] couldn't restrict {} to its owner: {}", path, e.toString());
			}
			// A shared arena: the rings are written from the render, server and collision threads.
			// It is never closed; the mapping lives as long as Minecraft does.
			MemorySegment seg = channel.map(FileChannel.MapMode.READ_WRITE, 0, MAPPING_BYTES, Arena.ofShared());
			Runtime.getRuntime().addShutdownHook(new Thread(() -> this.cleanUp(path, seg), "LibertyCraft bridge cleanup"));
			LibertyCraft.LOG.info("[LibertyCraft] mapped {} ({} MiB)", path, MAPPING_BYTES >> 20);
			return seg;
		} catch (IOException e) {
			if (!this.warnedMissing) {
				this.warnedMissing = true;
				LibertyCraft.LOG.error("[LibertyCraft] can't map {}: {}", path, e.toString());
			}
			return null;
		}
	}

	/**
	 * tmpfs pages stay in RAM until the file is deleted, and the overlay and render rings touch
	 * about 160 MiB of them. Remove the file when we exit, unless GTA IV is still using it (it may
	 * start a new Minecraft that must find the same inode).
	 */
	private void cleanUp(Path path, MemorySegment seg) {
		long beat = seg.get(JAVA_LONG, OFF_HEADER + H_HOST_HEARTBEAT);
		if (beat != 0 && this.nowMs() - beat < HEARTBEAT_TIMEOUT_MS) {
			return;
		}
		try {
			Files.deleteIfExists(path);
		} catch (IOException ignored) {
			// best effort, during shutdown
		}
	}

	private synchronized long nowNs() {
		try {
			int rc = (int) this.clockGettime.invokeExact(CLOCK_MONOTONIC_RAW, this.timespec);
			if (rc != 0) {
				throw new IllegalStateException("clock_gettime(CLOCK_MONOTONIC_RAW) failed");
			}
			return this.timespec.get(JAVA_LONG, 0) * 1_000_000_000L + this.timespec.get(JAVA_LONG, 8);
		} catch (Throwable t) {
			throw t instanceof RuntimeException r ? r : new IllegalStateException(t);
		}
	}

	@Override
	long nowMs() {
		return this.nowNs() / 1_000_000L;
	}

	@Override
	long qpcNow() {
		return this.nowNs() / 100L;
	}

	@Override
	long qpcFrequency0() {
		return QPC_FREQUENCY;
	}

	@Override
	boolean hostAlive(int hostPid) {
		// The header holds GTA IV's Wine pid, which means nothing to Linux; go by its heartbeat.
		return Link.active();
	}
}
