package dev.libertycraft.link;

/**
 * Mirror of protocol/libertycraft_protocol.h. Keep the two in sync.
 */
public final class Proto {
	private Proto() {
	}

	public static final int MAGIC = 0x5954424C; // "LBTY"
	public static final int VERSION = 11;
	// Where the two processes meet. On Linux (Minecraft native, GTA IV under Wine/Proton) it is a
	// plain file in tmpfs that both sides mmap; GTA IV reaches it as Z:\dev\shm\libertycraft-bridge.
	// On native Windows it is the named section below instead. -Dlibertycraft.link=<path or name>
	// points a second client (multiplayer testing) at its own stand-in host.
	public static final String BRIDGE_FILE = System.getProperty("libertycraft.link", "/dev/shm/libertycraft-bridge");
	public static final String MAPPING_NAME = System.getProperty("libertycraft.link", "Local\\LibertyCraft_v1");
	public static final double UNITS_PER_BLOCK = 1.0; // GTA IV metres per block (SkyCraft: 70 Skyrim units)

	public static final long OFF_HEADER = 0x0;
	public static final long OFF_SKY_STATE = 0x100;
	public static final long OFF_MC_STATE = 0x200;
	public static final long OFF_WATER_GRID = 0x400;
	public static final int WATER_GRID_SIZE = 16;
	public static final long WG_SEQ = 0x0, WG_ORIGIN_X = 0x4, WG_ORIGIN_Z = 0x8, WG_WORLD_ID = 0xC, WG_SURFACE = 0x10;
	/** WaterGrid worldId flag: a tile of the lattice around the player (origins multiples of 16), not the grid around them. */
	public static final int WATER_GRID_TILE = 0x80000000;
	public static final long OFF_OVERLAY_CTL = 0x300;
	public static final long OFF_OVERLAY_SLOT_HDR = 0x340;
	public static final long OFF_INPUT_RING = 0x1000;
	public static final long OFF_COLLISION_RING = 0x20000;
	public static final long COLLISION_RING_BYTES = 32L << 20;
	public static final long OFF_OVERLAY_PIXELS = OFF_COLLISION_RING + COLLISION_RING_BYTES;
	public static final int MAX_OVERLAY_W = 3840;
	public static final int MAX_OVERLAY_H = 2160;
	public static final long OVERLAY_SLOT_BYTES = (long) MAX_OVERLAY_W * MAX_OVERLAY_H * 4;
	public static final int OVERLAY_SLOTS = 3;
	public static final long OFF_ACTOR_TABLE = 0x12000;
	public static final long OFF_EVENT_RING = 0x17000;
	public static final long OFF_WORLD_ENTITIES = 0x1C000;
	public static final long OFF_RENDER_RING = OFF_OVERLAY_PIXELS + OVERLAY_SLOT_BYTES * OVERLAY_SLOTS;
	public static final long RENDER_RING_BYTES = 64L << 20;
	public static final long MAPPING_BYTES = OFF_RENDER_RING + RENDER_RING_BYTES;

	// Input types added in v5
	public static final int IN_HURT = 7;
	public static final int IN_OPEN_MENU = 8;
	/** LibertyCraft: a GTA IV bullet stopped at a block (kInBulletImpact): code = face, a, b, c = position * 256. */
	public static final int IN_BULLET_IMPACT = 9;
	/** LibertyCraft: GTA IV brought the player back after dying, at a hospital (kInRestore): full health and food. */
	public static final int IN_RESTORE = 10;
	/**
	 * LibertyCraft: a GTA IV bullet hit a Minecraft mob (kInMobHit): code = GTA IV weapon type, a = the mob's
	 * entity id, b = the shooter's actor formId (0 unknown, {@link #MOB_HIT_BY_PLAYER} the player), c = Minecraft damage * 100.
	 */
	public static final int IN_MOB_HIT = 11;
	public static final int MOB_HIT_BY_PLAYER = -1;
	/**
	 * LibertyCraft: one of GTA IV's own explosions (kInGtaExplosion): a, b, c = MC position * 256, code = GTA type
	 * (GTA_BLAST_TYPE_MASK) | radius in half metres (GTA_BLAST_RADIUS_SHIFT, 7 bits) | GTA_BLAST_BY_PLAYER | GTA_BLAST_FIRE.
	 */
	public static final int IN_GTA_EXPLOSION = 12;
	public static final int GTA_BLAST_TYPE_MASK = 0x1F, GTA_BLAST_RADIUS_SHIFT = 5, GTA_BLAST_BY_PLAYER = 1 << 12, GTA_BLAST_FIRE = 1 << 13;
	/** LibertyCraft: the mover broke one of GTA IV's props (kInPropHit): code = percent of its speed it keeps (PROP_KEEP_MASK), a/b/c = the prop, MC * 256. */
	public static final int IN_PROP_HIT = 13;
	public static final int PROP_KEEP_MASK = 0x7F;
	public static final int HURT_MELEE = 0;
	public static final int HURT_PROJECTILE = 1;
	public static final int HURT_MAGIC = 2;
	public static final int HURT_OTHER = 3;
	public static final int HURT_BLOCKED_IN_HOST = 1;
	public static final int HURT_POWER_ATTACK = 2;
	/** LibertyCraft: bits 16 to 24 hold the MC yaw (degrees) from the player toward what hurt it (kHurtHasDirection). */
	public static final int HURT_HAS_DIRECTION = 1 << 2;
	public static final int HURT_DIRECTION_SHIFT = 16;

	// Actor table (relative to OFF_ACTOR_TABLE)
	public static final int MAX_ACTORS = 256;
	public static final long AT_SEQ = 0x00;
	public static final long AT_COUNT = 0x04;
	/** LibertyCraft: when the host wrote the table (QueryPerformanceCounter, 100 ns units; 0: unknown). */
	public static final long AT_STAMP = 0x08;
	public static final long AT_RECORDS = 0x40;
	public static final long ACTOR_RECORD_BYTES = 64;
	public static final int ACTOR_HOSTILE = 1;
	public static final int ACTOR_DEAD = 1 << 1;
	public static final int ACTOR_ESSENTIAL = 1 << 2;
	public static final int ACTOR_IN_COMBAT = 1 << 3;
	/**
	 * LibertyCraft: the record is one piece of a GTA IV vehicle (up to {@link #ACTOR_VEHICLE_SEGMENTS}
	 * along its length), formId = {@link #ACTOR_VEHICLE_TAG} | (vehicle handle << 4) | piece.
	 */
	public static final int ACTOR_VEHICLE = 1 << 4;
	public static final int ACTOR_VEHICLE_TAG = 0x56000000;
	public static final int ACTOR_VEHICLE_SEGMENTS = 16;
	/** The piece bits of a vehicle record's formId; {@code formId & ~ACTOR_VEHICLE_PIECE_MASK} names the vehicle. */
	public static final int ACTOR_VEHICLE_PIECE_MASK = 0xF;
	/**
	 * LibertyCraft: with {@link #ACTOR_VEHICLE}, a piece of the vehicle the player sits in (kActorPlayerVehicle).
	 * No stand-in: it only runs mobs over (as the player) and pushes them aside.
	 */
	public static final int ACTOR_PLAYER_VEHICLE = 1 << 5;
	/** LibertyCraft: with {@link #ACTOR_VEHICLE}, someone sits in it (kActorOccupied): hostile mobs go for it. */
	public static final int ACTOR_OCCUPIED = 1 << 6;
	/** LibertyCraft: a mission character, or a vehicle one sits in (kActorMission): Minecraft's mobs leave it alone. */
	public static final int ACTOR_MISSION = 1 << 7;

	// Event ring (relative to OFF_EVENT_RING)
	public static final int EVENT_RING_ENTRIES = 512;
	public static final long ER_HEAD = 0x00;
	public static final long ER_TAIL = 0x40;
	public static final long ER_DATA = 0x80;
	public static final long EVENT_BYTES = 32;
	public static final int EV_HIT_ACTOR = 1;
	public static final int EV_PLAYER_DIED = 2;
	public static final int EV_EXPLOSION = 3;
	/**
	 * LibertyCraft: EV_EXPLOSION flags (kExplosionFirework): a firework rocket with stars burst; then
	 * a/b/c = where (MC), d = the blast radius (blocks), formId = the stand-in it struck (0: none),
	 * weapon = its star count.
	 */
	public static final int EXPLOSION_FIREWORK = 1;
	/** LibertyCraft: EV_EXPLOSION flags (kExplosionByPlayer): the player launched it. */
	public static final int EXPLOSION_BY_PLAYER = 1 << 1;
	/** LibertyCraft: EV_EXPLOSION flags (kExplosionByMob): a mob caused it (a creeper, a ghast's fireball): no crime of the player's. */
	public static final int EXPLOSION_BY_MOB = 1 << 2;
	public static final int EV_ARROW_STUCK = 4;
	public static final int EV_SKILL_USE = 5;
	/** LibertyCraft: where the next EV_HIT_ACTOR on the same vehicle piece landed (see kEvHitPoint). */
	public static final int EV_HIT_POINT = 6;
	/** LibertyCraft: the player ran into a ped's stand-in (kEvBump): a = speed m/s, b/c = motion dir x/z, d = overlap; flags BUMP_*. */
	public static final int EV_BUMP = 7;
	public static final int BUMP_SPRINTING = 1;
	public static final int BUMP_FLYING = 1 << 1;
	public static final int BUMP_NEW_CONTACT = 1 << 2;
	/**
	 * LibertyCraft: a hostile mob near the player (kEvMob), every 5 ticks: flags = entity id, formId = the ped it
	 * is after (0 none), a/b/c = feet, d = height, weapon = width in hundredths of a block | MOB_* flags.
	 */
	public static final int EV_MOB = 8;
	public static final int MOB_AFTER_PLAYER = 1 << 16;
	public static final double MOB_RANGE = 80.0;
	public static final int MAX_MOBS = 32;
	/** LibertyCraft: Minecraft's time of day was set (kEvSetTime): a = GTA IV's hour for it (0 to 24, minutes as the fraction). */
	public static final int EV_SET_TIME = 9;
	/** LibertyCraft: Minecraft's weather was set (kEvSetWeather): formId = GTA_* weather type, a = seconds (0: until told otherwise). */
	public static final int EV_SET_WEATHER = 10;
	/**
	 * LibertyCraft: a mob hit the player while GTA IV drives him on foot (kEvMobHitPlayer): a = MC damage, b/c = push
	 * direction, d = push strength beyond a plain blow's, flags = HIT_*, weapon = WEAPON_*.
	 */
	public static final int EV_MOB_HIT_PLAYER = 11;
	/**
	 * LibertyCraft: an elytra crash (kEvImpact): a = the crash's speed (m/s), b/c = horizontal flight direction x/z,
	 * d = horizontal speed before (m/s), flags = IMPACT_*.
	 */
	public static final int EV_IMPACT = 12;
	public static final int IMPACT_WALL = 1, IMPACT_GROUND = 1 << 1;
	/**
	 * LibertyCraft: the player moving fast (kEvMover), every tick from MOVER_MIN_SPEED: a/b/c = the moving box's
	 * bottom centre, d = speed (m/s), formId = MOVER_* | width cm << MOVER_WIDTH_SHIFT | height cm <<
	 * MOVER_HEIGHT_SHIFT, flags / weapon = the motion's yaw / pitch (float bits, MC degrees).
	 */
	public static final int EV_MOVER = 13;
	public static final int MOVER_FLYING = 1, MOVER_RIDING = 1 << 1, MOVER_SPRINTING = 1 << 2;
	public static final int MOVER_WIDTH_SHIFT = 8, MOVER_HEIGHT_SHIFT = 18;
	public static final double MOVER_MIN_SPEED = 5.0;
	// SkyCraft's Skyrim skills (ActorValue). Kept for protocol parity; LibertyCraft never sends
	// EV_SKILL_USE because GTA IV has no skill XP to feed.
	public static final int SKILL_BLOCK = 9;
	public static final int SKILL_SMITHING = 10;
	public static final int SKILL_HEAVY_ARMOR = 11;
	public static final int SKILL_LIGHT_ARMOR = 12;
	public static final int HIT_CRITICAL = 1;
	public static final int HIT_PROJECTILE = 1 << 1;
	public static final int HIT_SWEEP = 1 << 2;
	public static final int HIT_FIRE = 1 << 3;
	/** LibertyCraft: an explosion hurt the stand-in (the host's own blast already hits the real thing). */
	public static final int HIT_EXPLOSION = 1 << 4;
	/** LibertyCraft: mobs dealt all of this hit, the player none of it (kHitByMob): no crime, the ped doesn't turn on the player. */
	public static final int HIT_BY_MOB = 1 << 5;
	public static final int WEAPON_UNARMED = 0;
	public static final int WEAPON_BLADE = 1;
	public static final int WEAPON_AXE = 2;
	public static final int WEAPON_BLUNT = 3;
	public static final int WEAPON_PIERCE = 4;
	public static final int WEAPON_ARROW = 5;

	// World entities (relative to OFF_WORLD_ENTITIES)
	public static final int MAX_WORLD_ENTITIES = 160;
	public static final long WE_SEQ = 0x00;
	public static final long WE_COUNT = 0x04;
	public static final long WE_HAS_SELECTION = 0x08;
	public static final long WE_SEL_MIN = 0x0C;
	public static final long WE_SEL_MAX = 0x18;
	public static final long WE_RECORDS = 0x40;
	public static final long WORLD_ENTITY_BYTES = 96;
	public static final int WE_ARROW = 1;
	public static final int WE_ITEM = 2;
	public static final int WE_TRIDENT = 3;
	public static final int WE_BLOCK = 4;
	public static final int WE_CRACK = 5;
	public static final int WE_SHADOW = 6;

	// Render ring (relative to OFF_RENDER_RING)
	public static final long RR_HEAD = 0x00;
	public static final long RR_TAIL = 0x40;
	public static final long RR_DATA = 0x80;
	public static final long RR_DATA_BYTES = RENDER_RING_BYTES - RR_DATA;
	public static final int REN_PAD = 0;
	public static final int REN_ATLAS = 1;
	public static final int REN_SECTION = 2;
	public static final int REN_CLEAR_ALL = 3;
	public static final int REN_TEXTURE = 4;
	public static final int REN_AVATAR = 5;
	public static final int REN_SCENE = 6;
	public static final int REN_ATLAS_REGION = 7;
	public static final int REN_LIGHTS = 8;
	public static final int REN_RAGDOLL = 9;
	public static final int REN_SOLIDS = 10;
	public static final int REN_DUG = 11;
	/** LibertyCraft: RenLiquids (sx, sy, sz, count) + count x {x, y, z, info}: info bits 0-3 the surface in fifteenths, bits 4-5 LIQUID_*. */
	public static final int REN_LIQUIDS = 12;
	public static final int LIQUID_WATER = 1, LIQUID_LAVA = 2;
	public static final int PART_HEAD = 1, PART_BODY = 2, PART_RIGHT_ARM = 3, PART_LEFT_ARM = 4, PART_RIGHT_LEG = 5, PART_LEFT_LEG = 6;
	/** kRenRagdoll RenBatch flags bit 12: a held item (on its arm's part). */
	public static final int RAGDOLL_HELD = 1 << 12;
	/** LibertyCraft: kRenRagdoll RenBatch flags bit 13: a cape or an elytra (on the body part), hidden by a host posing the body seated. */
	public static final int RAGDOLL_BACK = 1 << 13;
	public static final int LIGHT_STEADY = 0, LIGHT_FLAME = 1, LIGHT_LAVA = 2;
	public static final int REN_VERTEX_BYTES = 32;

	// Header
	public static final long H_MAGIC = 0x00;
	public static final long H_VERSION = 0x04;
	public static final long H_HOST_PID = 0x08;
	public static final long H_MC_PID = 0x0C;
	public static final long H_HOST_HEARTBEAT = 0x10;
	public static final long H_MC_HEARTBEAT = 0x18;

	// SkyState (relative to OFF_SKY_STATE)
	public static final long SS_SEQ = 0x00;
	public static final long SS_FLAGS = 0x04;
	public static final long SS_WORLD_ID = 0x08;
	public static final long SS_COLLISION_EPOCH = 0x0C;
	public static final long SS_POS_X = 0x10;
	public static final long SS_POS_Y = 0x18;
	public static final long SS_POS_Z = 0x20;
	public static final long SS_YAW = 0x28;
	public static final long SS_PITCH = 0x2C;
	public static final long SS_TELEPORT_SEQ = 0x30;
	public static final long SS_VIEWPORT_W = 0x34;
	public static final long SS_VIEWPORT_H = 0x38;
	public static final long SS_GAME_HOUR = 0x3C;

	public static final int SKY_IN_GAME = 1;
	public static final int SKY_MENU_OPEN = 1 << 1;
	public static final int SKY_LOADING = 1 << 2;
	/** GTA IV controls the player (Niko mode, vehicle, cutscene): follow SkyState, don't simulate. */
	public static final int SKY_HOST_DRIVES = 1 << 3;
	/** With SKY_HOST_DRIVES: in a vehicle; pos is the rider's feet on the mount, yaw the vehicle heading. */
	public static final int SKY_IN_VEHICLE = 1 << 4;
	/** LibertyCraft: GTA IV shows a cutscene or a mission script's camera (kSkyScene): Minecraft pauses like for its menu. */
	public static final int SKY_SCENE = 1 << 5;
	/** LibertyCraft: a phone call is going on in GTA IV (kSkyPhoneCall): Minecraft's sounds duck. */
	public static final int SKY_PHONE_CALL = 1 << 6;
	/** LibertyCraft: SkyState flags bits 8 to 11 hold GTA IV's weather type + 1 (0: not sent); see GTA_WEATHER_*. */
	public static final int SKY_WEATHER_SHIFT = 8;
	public static final int SKY_WEATHER_MASK = 0xF << SKY_WEATHER_SHIFT;
	/** LibertyCraft: GTA IV's phone is out (kSkyPhoneOut): no first-person hands or held items meanwhile. */
	public static final int SKY_PHONE_OUT = 1 << 12;
	/** GTA IV's weather types (GtaWeather). */
	public static final int GTA_EXTRA_SUNNY = 0, GTA_SUNNY = 1, GTA_SUNNY_WINDY = 2, GTA_CLOUDY = 3, GTA_RAIN = 4, GTA_DRIZZLE = 5, GTA_FOGGY = 6,
		GTA_LIGHTNING = 7;

	// McState (relative to OFF_MC_STATE)
	public static final long MS_SEQ = 0x00;
	public static final long MS_FLAGS = 0x04;
	public static final long MS_X = 0x08;
	public static final long MS_Y = 0x10;
	public static final long MS_Z = 0x18;
	public static final long MS_YAW = 0x20;
	public static final long MS_PITCH = 0x24;
	public static final long MS_EYE_HEIGHT = 0x28;
	public static final long MS_SENSITIVITY = 0x2C;
	public static final long MS_TELEPORT_ACK = 0x30;
	public static final long MS_GUI_SCALE = 0x34;
	public static final long MS_FRAME_COUNTER = 0x38;
	public static final long MS_FOV = 0x40;
	public static final long MS_BOB_PHASE = 0x44;
	public static final long MS_BOB_AMOUNT = 0x48;
	public static final long MS_EYE_X = 0x50;
	public static final long MS_EYE_Y = 0x58;
	public static final long MS_EYE_Z = 0x60;
	public static final long MS_TICK_QPC = 0x68;
	public static final long MS_PREV_X = 0x70;
	public static final long MS_CUR_X = 0x88;
	public static final long MS_EYE_HEIGHT_O = 0xA0;
	public static final long MS_EYE_HEIGHT_T = 0xA4;
	public static final long MS_WALK_O = 0xA8;
	public static final long MS_WALK = 0xAC;
	public static final long MS_BOB_O = 0xB0;
	public static final long MS_BOB = 0xB4;
	public static final long MS_TICK_MS = 0xB8;
	public static final long MS_CAMERA_MODE = 0xC0;
	public static final long MS_CAMERA_DISTANCE = 0xC4;
	/** LibertyCraft, in McState's padding: health * 100 | max health * 100 << 16 (see kMcVitalsValid). */
	public static final long MS_VITALS_HEALTH = 0x4C;
	/** LibertyCraft, in McState's padding: armour points | absorption << 8 | MC_VITALS_VALID. */
	public static final long MS_VITALS_ARMOUR = 0xBC;
	public static final int MC_VITALS_VALID = 1 << 31;

	public static final int MC_IN_WORLD = 1;
	public static final int MC_SCREEN_OPEN = 1 << 1;
	public static final int MC_ON_GROUND = 1 << 2;
	public static final int MC_SNEAKING = 1 << 3;
	public static final int MC_SPRINTING = 1 << 4;
	public static final int MC_DEAD = 1 << 5;
	public static final int MC_SWIMMING = 1 << 6;
	public static final int MC_FLYING = 1 << 7;
	/** LibertyCraft: the player's shield is up and blocking (kMcBlocking). */
	public static final int MC_BLOCKING = 1 << 8;
	/** LibertyCraft: the player is in the blocky city; GTA IV hides its own map geometry (kMcBlockyCity). */
	public static final int MC_BLOCKY_CITY = 1 << 9;
	/** LibertyCraft: the player is in creative or spectator mode (invulnerable); GTA IV's peds can't drag it out of a vehicle (kMcCreative). */
	public static final int MC_CREATIVE = 1 << 10;

	// Overlay
	public static final long OC_STATE = 0x00;
	public static final long OC_FRAMES_PUBLISHED = 0x08;
	public static final int OVERLAY_DIRTY = 1 << 2;
	public static final long SLOT_HDR_SIZE = 0x40;
	public static final long SH_WIDTH = 0x00;
	public static final long SH_HEIGHT = 0x04;
	public static final long SH_FLAGS = 0x08;
	public static final long SH_FRAME_ID = 0x10;
	/** LibertyCraft: dirty tiles (kOverlayFlagTiles): the frame differs from SH_BASE_FRAME only in the SH_TILES tiles. */
	public static final int OVERLAY_FLAG_TILES = 1 << 1;
	public static final int OVERLAY_TILE_GRID = 16;
	public static final long SH_BASE_FRAME = 0x18;
	public static final long SH_TILES = 0x20;

	// Input ring (relative to OFF_INPUT_RING)
	public static final int INPUT_RING_ENTRIES = 4096;
	public static final long IR_HEAD = 0x00;
	public static final long IR_TAIL = 0x40;
	public static final long IR_DATA = 0x80;
	public static final int IN_KEY = 1;
	public static final int IN_MOUSE_BUTTON = 2;
	public static final int IN_SCROLL = 3;
	public static final int IN_CURSOR = 4;
	public static final int IN_TEXT = 5;
	public static final int IN_RELEASE_ALL = 6;

	// Collision ring (relative to OFF_COLLISION_RING)
	public static final long CR_HEAD = 0x00;
	public static final long CR_TAIL = 0x40;
	public static final long CR_DATA = 0x80;
	public static final long CR_DATA_BYTES = COLLISION_RING_BYTES - CR_DATA;
	public static final int COL_PAD = 0;
	public static final int COL_CLEAR = 1;
	public static final int COL_REGION = 2;
	public static final int COL_TRIS = 3;
	/** LibertyCraft: GTA IV forgot a column of regions (kColForget); Minecraft may drop it. */
	public static final int COL_FORGET = 4;
	public static final int COL_TRI_BYTES = 40;
	public static final int TRI_STAIR_HELPER = 1;
	public static final int TRI_DIGGABLE = 2;
	public static final int TRI_GHOST = 4;
	public static final int TRI_TERRAIN = 8;
	/** LibertyCraft: bits 16-23 hold GTA IV's material of the surface (materials.dat index, kTriGtaMaterial). */
	public static final int TRI_GTA_MATERIAL = 1 << 4;
	public static final int TRI_GTA_MATERIAL_SHIFT = 16;
	public static final int TRI_MATERIAL_SHIFT = 8;
	// DigMaterial (libertycraft_protocol.h)
	public static final int DIG_NONE = 0, DIG_GRASS = 1, DIG_DIRT = 2, DIG_STONE = 3, DIG_COBBLE = 4, DIG_SNOW = 5, DIG_ICE = 6, DIG_SAND = 7,
		DIG_GRAVEL = 8, DIG_MUD = 9, DIG_OAK_LOG = 10, DIG_SPRUCE_LOG = 11, DIG_BIRCH_LOG = 12, DIG_PLANKS = 13, DIG_METAL = 14, DIG_GLASS = 15,
		DIG_ORGANIC = 16, DIG_CLOTH = 17, DIG_BONE = 18, DIG_WEB = 19, DIG_ASH = 20, DIG_BEDROCK = 21, DIG_MATERIAL_COUNT = 22;
	public static final int COL_REGION_HEADER_BYTES = 32;
	public static final int COL_BLOCK_BYTES = 80;
}
