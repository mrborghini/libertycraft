package dev.libertycraft.client;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import dev.libertycraft.LibertyCraft;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.net.StandardProtocolFamily;
import java.net.UnixDomainSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import net.minecraft.client.Minecraft;
import net.minecraft.network.chat.ClickEvent;
import net.minecraft.network.chat.Component;
import org.jspecify.annotations.Nullable;

/**
 * Discord Rich Presence: the player's Discord status shows LibertyCraft (solo, hosting, or in a friend's
 * world), and while hosting it carries a Join button. A friend with LibertyCraft running who clicks it
 * gets the host's e4mc link here, and joins exactly as with /join.
 *
 * <p>Talks to the Discord app on this PC over its local IPC endpoint from one background thread:
 * the named pipe {@code \\.\pipe\discord-ipc-N} on Windows, the Unix socket
 * {@code $XDG_RUNTIME_DIR/discord-ipc-N} (or a Flatpak/Snap variant of it) on Linux. Frames are
 * sent, and only those that have fully arrived are read, so a read never blocks a write. Without
 * Discord running it quietly retries now and then.
 */
public final class DiscordPresence {
	// A Discord application id (developer portal; public, not a secret). LibertyCraft has none yet,
	// and SkyCraft's would show up as "SkyCraft", so presence is off unless one is passed with
	// -Dlibertycraft.discordAppId=<id> (its art assets need a "libertycraft" image).
	private static final String APP_ID = System.getProperty("libertycraft.discordAppId", "0");
	// e4mc's addresses, as it prints them in chat (visible, or only in its click-to-copy action).
	private static final Pattern E4MC_LINK = Pattern.compile("[a-z0-9-]+(?:\\.[a-z0-9-]+)*\\.e4mc\\.link", Pattern.CASE_INSENSITIVE);
	private static final int OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4;
	private static final long START = System.currentTimeMillis() / 1000L;

	private static volatile @Nullable String hostLink;       // our world's e4mc link while it's open to LAN
	private static volatile @Nullable String wantedActivity; // SET_ACTIVITY args, as the render thread last described them
	private static long nextDescribe;

	private DiscordPresence() {
	}

	public static void start() {
		if ("0".equals(APP_ID) || APP_ID.isBlank()) {
			LibertyCraft.LOG.info("[LibertyCraft] Discord Rich Presence off (no application id)");
			return;
		}
		Thread thread = new Thread(DiscordPresence::run, "LibertyCraft Discord");
		thread.setDaemon(true);
		thread.start();
	}

	/** Every chat line (ChatComponentMixin): e4mc's "Local game hosted on domain [...]" gives the link. */
	public static void onChat(Component message) {
		String link = findLink(message);
		if (link != null) {
			hostLink = link;
			LibertyCraft.LOG.info("[LibertyCraft] this world is open to friends at {}", link);
		}
	}

	private static @Nullable String findLink(Component message) {
		Matcher m = E4MC_LINK.matcher(message.getString());
		if (m.find()) {
			return m.group().toLowerCase();
		}
		for (Component part : message.toFlatList()) {
			if (part.getStyle().getClickEvent() instanceof ClickEvent.CopyToClipboard copy) {
				m = E4MC_LINK.matcher(copy.value());
				if (m.find()) {
					return m.group().toLowerCase();
				}
			}
		}
		return null;
	}

	/** Render thread, every client tick: what the status should say now. */
	public static void tick(Minecraft minecraft) {
		long now = System.currentTimeMillis();
		if (now < nextDescribe) {
			return;
		}
		nextDescribe = now + 1000;
		var server = minecraft.getSingleplayerServer();
		if (server == null || !server.isPublished()) {
			hostLink = null;
		}
		JsonObject activity = null;
		// While in a world, whatever GTA IV is doing: Alt-Tabbed, GTA IV pauses and the link goes quiet.
		if (minecraft.player != null && minecraft.getConnection() != null) {
			int players = minecraft.getConnection().getOnlinePlayers().size();
			String friend = MirrorWorld.friendAddress(minecraft);
			activity = new JsonObject();
			activity.addProperty("details", "Playing GTA IV as a Minecraft player");
			JsonObject timestamps = new JsonObject();
			timestamps.addProperty("start", START);
			activity.add("timestamps", timestamps);
			JsonObject assets = new JsonObject();
			assets.addProperty("large_image", "libertycraft");
			assets.addProperty("large_text", "LibertyCraft");
			activity.add("assets", assets);
			String joinable = hostLink != null ? hostLink : friend;
			if (hostLink != null) {
				activity.addProperty("state", "Hosting a world");
			} else if (friend != null) {
				activity.addProperty("state", "In a friend's world");
			} else {
				activity.addProperty("state", "Playing solo");
			}
			if (joinable != null) {
				// Anyone who can see the status may join the same world (its link is the secret).
				JsonObject party = new JsonObject();
				party.addProperty("id", "libertycraft-" + Integer.toHexString(joinable.hashCode()));
				JsonArray size = new JsonArray();
				size.add(Math.max(players, 1));
				// A friend's world: LibertyCraft hosts take 100 (IntegratedServerMixin); a guest can't ask.
				size.add(server != null ? Math.max(server.getMaxPlayers(), players + 1) : Math.max(100, players + 1));
				party.add("size", size);
				activity.add("party", party);
				JsonObject secrets = new JsonObject();
				secrets.addProperty("join", joinable);
				activity.add("secrets", secrets);
			}
		}
		JsonObject args = new JsonObject();
		args.addProperty("pid", ProcessHandle.current().pid());
		args.add("activity", activity);
		wantedActivity = args.toString();
	}

	// ---- the IPC thread --------------------------------------------------------------------------

	private static @Nullable Conn pipe;
	// Bytes received but not yet parsed into whole frames.
	private static ByteBuffer inbox = ByteBuffer.allocate(4096).order(ByteOrder.LITTLE_ENDIAN);
	private static @Nullable String sentActivity;
	private static long lastSent;
	private static boolean loggedNoDiscord;
	private static int nonce;

	private static void run() {
		while (true) {
			try {
				if (pipe == null && !connect()) {
					Thread.sleep(15_000);
					continue;
				}
				String wanted = wantedActivity;
				long now = System.currentTimeMillis();
				// Discord accepts 5 updates per 20 s; stay well under.
				if (wanted != null && !wanted.equals(sentActivity) && now - lastSent > 5_000) {
					command("SET_ACTIVITY", JsonParser.parseString(wanted).getAsJsonObject(), null);
					sentActivity = wanted;
					lastSent = now;
				}
				readAvailable();
				Thread.sleep(250);
			} catch (IOException e) {
				LibertyCraft.LOG.info("[LibertyCraft] Discord connection closed ({}); retrying later", e.getMessage());
				close();
			} catch (InterruptedException e) {
				return;
			} catch (RuntimeException e) {
				LibertyCraft.LOG.warn("[LibertyCraft] Discord Rich Presence error", e);
				close();
			}
		}
	}

	private static boolean connect() throws IOException, InterruptedException {
		for (String endpoint : endpoints()) {
			try {
				pipe = WINDOWS ? new PipeConn(endpoint) : new SocketConn(endpoint);
			} catch (IOException e) {
				continue;
			}
			inbox.clear();
			JsonObject hello = new JsonObject();
			hello.addProperty("v", 1);
			hello.addProperty("client_id", APP_ID);
			write(OP_HANDSHAKE, hello);
			// READY, then subscribe to friends clicking Join.
			long deadline = System.currentTimeMillis() + 5_000;
			while (System.currentTimeMillis() < deadline) {
				JsonObject msg = readFrame();
				if (msg != null && "READY".equals(str(msg, "evt"))) {
					command("SUBSCRIBE", null, "ACTIVITY_JOIN");
					LibertyCraft.LOG.info("[LibertyCraft] connected to Discord (Rich Presence)");
					loggedNoDiscord = false;
					sentActivity = null;
					return true;
				}
				if (msg == null) {
					Thread.sleep(100);
				}
			}
			close();
			return false;
		}
		if (!loggedNoDiscord) {
			loggedNoDiscord = true;
			LibertyCraft.LOG.info("[LibertyCraft] Discord isn't running; Rich Presence will connect when it is");
		}
		return false;
	}

	private static void command(String cmd, @Nullable JsonObject args, @Nullable String evt) throws IOException {
		JsonObject frame = new JsonObject();
		frame.addProperty("cmd", cmd);
		frame.addProperty("nonce", Integer.toString(++nonce));
		if (args != null) {
			frame.add("args", args);
		}
		if (evt != null) {
			frame.addProperty("evt", evt);
		}
		write(OP_FRAME, frame);
	}

	private static void readAvailable() throws IOException {
		JsonObject msg;
		while ((msg = readFrame()) != null) {
			if ("DISPATCH".equals(str(msg, "cmd")) && "ACTIVITY_JOIN".equals(str(msg, "evt")) && msg.has("data")) {
				String secret = str(msg.getAsJsonObject("data"), "secret");
				if (secret != null && E4MC_LINK.matcher(secret).matches()) {
					LibertyCraft.LOG.info("[LibertyCraft] joining {} from Discord", secret);
					Minecraft.getInstance().execute(() -> MirrorWorld.joinFriend(Minecraft.getInstance(), secret));
				}
			} else if ("ERROR".equals(str(msg, "evt"))) {
				LibertyCraft.LOG.warn("[LibertyCraft] Discord says {}", msg.get("data"));
			}
		}
	}

	private static void write(int op, JsonObject payload) throws IOException {
		byte[] body = payload.toString().getBytes(StandardCharsets.UTF_8);
		ByteBuffer frame = ByteBuffer.allocate(8 + body.length).order(ByteOrder.LITTLE_ENDIAN);
		frame.putInt(op).putInt(body.length).put(body);
		pipe.write(frame.array());
	}

	/** The next frame if it has fully arrived (never blocks for one that hasn't). */
	private static @Nullable JsonObject readFrame() throws IOException {
		while (true) {
			pipe.readAvailable();
			if (inbox.position() < 8) {
				return null;
			}
			int op = inbox.getInt(0);
			int length = inbox.getInt(4);
			if (length < 0 || length > 1 << 24) {
				throw new IOException("bad frame length " + length);
			}
			if (inbox.position() < 8 + length) {
				if (inbox.capacity() < 8 + length) {
					inbox = ByteBuffer.allocate(8 + length).order(ByteOrder.LITTLE_ENDIAN).put(inbox.flip());
				}
				return null;
			}
			byte[] body = new byte[length];
			inbox.get(8, body);
			inbox.flip().position(8 + length);
			inbox.compact();
			String text = new String(body, StandardCharsets.UTF_8);
			switch (op) {
				case OP_PING -> {
					ByteBuffer pong = ByteBuffer.allocate(8 + length).order(ByteOrder.LITTLE_ENDIAN);
					pong.putInt(OP_PONG).putInt(length).put(body);
					pipe.write(pong.array());
				}
				case OP_CLOSE -> throw new IOException("Discord closed the connection: " + text);
				case OP_FRAME -> {
					return JsonParser.parseString(text).getAsJsonObject();
				}
				default -> {
				}
			}
		}
	}

	private static @Nullable String str(JsonObject o, String key) {
		return o.has(key) && o.get(key).isJsonPrimitive() ? o.get(key).getAsString() : null;
	}

	private static void close() {
		try {
			if (pipe != null) {
				pipe.close();
			}
		} catch (IOException ignored) {
		}
		pipe = null;
		inbox.clear();
		sentActivity = null;
	}

	// ---- the IPC endpoint, per OS --------------------------------------------------------------

	private static final boolean WINDOWS = System.getProperty("os.name", "").startsWith("Windows");

	/** Where Discord may be listening, in the order the official SDKs try them. */
	private static List<String> endpoints() {
		List<String> out = new ArrayList<>();
		if (WINDOWS) {
			for (int i = 0; i < 10; i++) {
				out.add("\\\\.\\pipe\\discord-ipc-" + i);
			}
			return out;
		}
		String base = System.getenv("XDG_RUNTIME_DIR");
		if (base == null || base.isBlank()) {
			base = System.getenv("TMPDIR");
		}
		if (base == null || base.isBlank()) {
			base = "/tmp";
		}
		// Native, then the sandboxed packages (Flatpak, Snap, and the "app" dir some forks use).
		String[] dirs = {"", "app/com.discordapp.Discord", ".flatpak/com.discordapp.Discord/xdg-run", "snap.discord", "app"};
		for (String dir : dirs) {
			for (int i = 0; i < 10; i++) {
				Path socket = Path.of(base, dir, "discord-ipc-" + i);
				if (Files.exists(socket)) {
					out.add(socket.toString());
				}
			}
		}
		return out;
	}

	/** One open connection to Discord. Reads go into {@link #inbox} and never block. */
	private interface Conn {
		void write(byte[] bytes) throws IOException;

		void readAvailable() throws IOException;

		void close() throws IOException;
	}

	/** Windows named pipe: a file; {@code available()} says how much can be read without blocking. */
	private static final class PipeConn implements Conn {
		private final RandomAccessFile file;
		private final FileInputStream in;

		PipeConn(String path) throws IOException {
			this.file = new RandomAccessFile(path, "rw");
			this.in = new FileInputStream(this.file.getFD());
		}

		@Override
		public void write(byte[] bytes) throws IOException {
			this.file.write(bytes);
		}

		@Override
		public void readAvailable() throws IOException {
			int n = Math.min(this.in.available(), inbox.remaining());
			if (n > 0) {
				this.file.readFully(inbox.array(), inbox.arrayOffset() + inbox.position(), n);
				inbox.position(inbox.position() + n);
			}
		}

		@Override
		public void close() throws IOException {
			this.file.close();
		}
	}

	/** Unix domain socket, non-blocking: reads take what has arrived. */
	private static final class SocketConn implements Conn {
		private final SocketChannel channel;

		SocketConn(String path) throws IOException {
			this.channel = SocketChannel.open(StandardProtocolFamily.UNIX);
			try {
				this.channel.connect(UnixDomainSocketAddress.of(path));
				this.channel.configureBlocking(false);
			} catch (IOException e) {
				this.channel.close();
				throw e;
			}
		}

		@Override
		public void write(byte[] bytes) throws IOException {
			ByteBuffer buf = ByteBuffer.wrap(bytes);
			long deadline = System.currentTimeMillis() + 5_000;
			while (buf.hasRemaining()) {
				if (this.channel.write(buf) == 0) {
					// Socket buffer full (frames are tiny, so Discord has stopped reading).
					if (System.currentTimeMillis() > deadline) {
						throw new IOException("Discord stopped reading");
					}
					Thread.onSpinWait();
				}
			}
		}

		@Override
		public void readAvailable() throws IOException {
			if (inbox.hasRemaining() && this.channel.read(inbox) < 0) {
				throw new IOException("Discord closed the socket");
			}
		}

		@Override
		public void close() throws IOException {
			this.channel.close();
		}
	}
}
