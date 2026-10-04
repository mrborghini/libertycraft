package dev.libertycraft.world.city;

/**
 * The blocky copy of Liberty City, one block at a time: what GTA IV's streamed collision says about a
 * block (its 1/8-block voxels from kColRegion, the materials of the kColTris triangles through it),
 * packed into one {@code long} for the store ({@link CityStore}), and the rules that turn that into a
 * Minecraft block ({@link CityChunkGenerator}). Pure Java, no Minecraft classes: unit tested.
 *
 * <p>GTA IV's collision arrives as surfaces, not solids (see asi/src/collision/Geometry.h): the
 * ground is filled 2 blocks down under its surface, slabs their thickness, sheets (roofs, awnings)
 * one voxel, walls and ceilings are shells a voxel or two thick. So "mostly filled" alone would lose
 * every wall: a block is solid when it is at least half filled, or a wall-like sheet covers most of
 * one of its vertical faces, or a floor-like sheet covers most of its footprint. A floor sheet in the
 * lower part of its block rounds down (the block under it is the solid one), so surfaces land on the
 * nearest whole block.
 */
public final class CityCells {
	private CityCells() {
	}

	// ---- a block's descriptor (a long) -----------------------------------------------------------
	// bits 0-9 voxel count (0..512), 10-12 highest voxel layer, 13-15 lowest voxel layer,
	// 16-22 / 23-29 / 30-36 how many of the 64 cells of the YZ / XZ / XY plane the voxels cover
	// (projections along x, y, z), 40-47 floor material, 48-55 wall material (MAT_*).
	private static final int TOP_SHIFT = 10, BOTTOM_SHIFT = 13, PX_SHIFT = 16, PY_SHIFT = 23, PZ_SHIFT = 30;
	private static final int FLOOR_SHIFT = 40, WALL_SHIFT = 48;

	/** No material known. */
	public static final int MAT_NONE = 0xFF;
	/** GTA IV's land (a floor over 2 m of solid, ColTri kTriTerrain) without a GTA material. */
	public static final int MAT_TERRAIN = 0xF0;
	/** A floor that isn't land (a slab, a roof) without a GTA material. */
	public static final int MAT_FLOOR = 0xF1;
	/** A wall without a GTA material. */
	public static final int MAT_WALL = 0xF2;
	/** An underside (a ceiling) without a GTA material. */
	public static final int MAT_CEILING = 0xF3;
	/** Codes below this are GTA IV material indices (common/data/materials/materials.dat order). */
	public static final int MAT_GTA_LIMIT = 0xF0;

	// ---- classification ---------------------------------------------------------------------------
	public static final int EMPTY = 0;
	public static final int SOLID = 1;
	/** A floor sheet low in its block: the block below is the solid one. */
	public static final int SOLID_BELOW = 2;

	/** At least this many of the 512 voxels: solid (the ground's top block rounds at its middle). */
	public static final int FULL_MIN = 256;
	/** A vertical sheet covering at least this many of a vertical face's 64 cells: a wall. */
	public static final int WALL_MIN = 36;
	/** A horizontal sheet covering at least this many of the 64 footprint cells: a floor or roof. */
	public static final int SHEET_MIN = 36;
	/** A floor sheet whose top voxel layer is this or higher fills its own block, else the one below. */
	public static final int SHEET_UPPER = 3;

	/** The descriptor of one block from its 8 voxel layers ({@code layers[y]} bit {@code z * 8 + x}). */
	public static long describe(long[] layers) {
		int count = 0, top = -1, bottom = -1, px = 0, pz = 0;
		long py = 0;
		for (int y = 0; y < 8; y++) {
			long layer = layers[y];
			if (layer == 0) {
				continue;
			}
			count += Long.bitCount(layer);
			if (bottom < 0) {
				bottom = y;
			}
			top = y;
			py |= layer;
			int columns = 0;
			for (int z = 0; z < 8; z++) {
				int row = (int) (layer >>> (z * 8)) & 0xFF;
				if (row != 0) {
					px++; // cell (y, z) of the YZ plane
				}
				columns |= row;
			}
			pz += Integer.bitCount(columns); // cells (y, x) of the XY plane
		}
		if (count == 0) {
			return 0L;
		}
		return count | (long) top << TOP_SHIFT | (long) bottom << BOTTOM_SHIFT | (long) px << PX_SHIFT | (long) Long.bitCount(py) << PY_SHIFT
			| (long) pz << PZ_SHIFT | (long) MAT_NONE << FLOOR_SHIFT | (long) MAT_NONE << WALL_SHIFT;
	}

	public static long withMaterials(long d, int floor, int wall) {
		return d & ~(0xFFL << FLOOR_SHIFT | 0xFFL << WALL_SHIFT) | (long) (floor & 0xFF) << FLOOR_SHIFT | (long) (wall & 0xFF) << WALL_SHIFT;
	}

	public static int count(long d) {
		return (int) d & 0x3FF;
	}

	public static int top(long d) {
		return (int) (d >>> TOP_SHIFT) & 7;
	}

	public static int bottom(long d) {
		return (int) (d >>> BOTTOM_SHIFT) & 7;
	}

	public static int projX(long d) {
		return (int) (d >>> PX_SHIFT) & 0x7F;
	}

	public static int projY(long d) {
		return (int) (d >>> PY_SHIFT) & 0x7F;
	}

	public static int projZ(long d) {
		return (int) (d >>> PZ_SHIFT) & 0x7F;
	}

	public static int floorMaterial(long d) {
		return (int) (d >>> FLOOR_SHIFT) & 0xFF;
	}

	public static int wallMaterial(long d) {
		return (int) (d >>> WALL_SHIFT) & 0xFF;
	}

	/** EMPTY, SOLID or SOLID_BELOW (see the class comment). */
	public static int classify(long d) {
		int count = count(d);
		if (count == 0) {
			return EMPTY;
		}
		if (count >= FULL_MIN || Math.max(projX(d), projZ(d)) >= WALL_MIN) {
			return SOLID;
		}
		if (projY(d) >= SHEET_MIN) {
			return top(d) >= SHEET_UPPER ? SOLID : SOLID_BELOW;
		}
		return EMPTY;
	}

	// ---- materials ----------------------------------------------------------------------------------

	/** GTA IV's materials, in common/data/materials/materials.dat order (the index GTA IV uses). */
	static final String[] GTA_MATERIALS = (
		"DEFAULT NOFRICTION CAR_VOID CONCRETE CONCRETE_FREEWAY PAINTED_ROAD PLASTER_BOARD RUMBLE_STRIP TARMAC BREEZE_BLOCK MOSSY_ROCK "
		+ "ROCK STONE BRICK_COBBLE BRICK_WALL MARBLE PAVING_SLABS CERAMICS ROOFTILE RAIL_SLEEPER WOOD_BOARD WOOD_COUNTER WOOD_FENCE "
		+ "WOOD_HANDRAIL WOOD_SECTION WOOD_WALL_OLD WOODEN_GARAGE_DOOR LAMINATE POLISHED_COURT GRAVEL POTHOLE RAILWAY_GRAVEL SAND "
		+ "CLAY_COURT DIRT_DRY TWIGS MUD_SOFT GRASS SHORT_GRASS BUSHES FLOWERS LEAVES_PILE BARK_CHIPPING TREE_BARK_DARK TREE_BARK_LIGHT "
		+ "TREE_BARK_MED AIRCON_DUCT AIRCON_VENT BILLBOARD_FRAME CORRUGATED_IRON ELECTRICITY_BOX HOLLOW_METAL_PANEL HOLLOW_METAL_RAIL "
		+ "HOLLOW_RUST_METAL LEAD_ROOFING METAL_AWNING METAL_CELLAR METAL_DRAINAGE METAL_GARAGE_DOOR METAL_GRILL METAL_HELIPAD "
		+ "METAL_LATTICE METAL_MANHOLE METAL_RAILING METAL_ROLLER_DOOR METAL_TREAD_PLATE METAL_VENT_SUBWAY RAIL_CROSSING RAIL_TRACK "
		+ "ROLLER_DOOR SOLID_METAL_PANEL SOLID_RUST_METAL TREADPLATE_ON_WOOD GLASS_BRICK GLASS_MEDIUM GLASS_STRONG GLASS_WEAK PERSPEX "
		+ "SKYLIGHTS CAR_METAL CAR_PLASTIC WINDSCREEN CORRUGATED_PLASTIC FIBREGLASS PLASTIC TARPAULIN LINOLEUM CARPET FABRIC_CLOTH "
		+ "ROOFING_FELT RUG RUBBER PAPER CARDBOARD MATTRESS_FOAM PILLOW_FEATHERS POTHOLE_PUDDLE PUDDLES WATER PED BUTTOCKS THIGH_LEFT "
		+ "SHIN_LEFT FOOT_LEFT THIGH_RIGHT SHIN_RIGHT FOOT_RIGHT SPINE0 SPINE1 SPINE2 SPINE3 NECK HEAD CLAVICLE_LEFT UPPER_ARM_LEFT "
		+ "LOWER_ARM_LEFT HAND_LEFT CLAVICLE_RIGHT UPPER_ARM_RIGHT LOWER_ARM_RIGHT HAND_RIGHT LOWFRICTION_WHEEL HOLLOW_WOOD "
		+ "HOLLOW_PLASTIC HOLLOW_CARDBOARD HOLLOW_FIBREGLASS WINDSCREEN_WEAK WINDSCREEN_MED_WEAK WINDSCREEN_MED_STRONG WINDSCREEN_STRONG "
		+ "TVSCREEN VIDEOWALL POOLTABLE_SURFACE POOLTABLE_CUSHION POOLTABLE_BALL WINDSCREEN_SIDE_WEAK WINDSCREEN_SIDE_MED WINDSCREEN_REAR "
		+ "WINDSCREEN_FRONT FX_METAL_GAS_PIPE_FLAME FX_METAL_WATER_PIPE FX_METAL_STEAM_PIPE FX_METAL_OIL_GLUG FX_WOOD_WATER_GLUG "
		+ "FX_CERAMIC_WATER_HI_PRESSURE FX_METAL_ELECTRIC_SPARKS FX_SPARE_1 FX_SPARE_2 FX_SPARE_3 EMISSIVE_GLASS EMISSIVE_PLASTIC "
		+ "GLASS_STRONG_SHOOT_THRU GRASS_PATCHY GRASS_LONG CARPET_FABRIC POOLTABLE_POCKET").split(" ");

	/** The GTA IV material's name ("TARMAC"), or a description of a LibertyCraft code. */
	public static String materialName(int mat) {
		if (mat >= 0 && mat < GTA_MATERIALS.length) {
			return GTA_MATERIALS[mat];
		}
		return switch (mat) {
			case MAT_NONE -> "none";
			case MAT_TERRAIN -> "land";
			case MAT_FLOOR -> "floor";
			case MAT_WALL -> "wall";
			case MAT_CEILING -> "ceiling";
			default -> "gta#" + mat;
		};
	}

	/** Where a solid block sits in its column. */
	public enum Role {
		/** Nothing solid above it: the surface you walk on (its floor material counts first). */
		TOP,
		/** Solid above it too: inside a wall, or the ground under the surface. */
		INSIDE
	}

	/**
	 * The Minecraft block (a block id) for a solid block: its floor and wall materials (MAT_* or a GTA
	 * index), its role, and how deep under the column's surface it is (0 at the surface).
	 */
	public static String blockFor(int floorMat, int wallMat, Role role, int depth) {
		int mat = role == Role.TOP ? (floorMat != MAT_NONE ? floorMat : wallMat != MAT_NONE ? wallMat : MAT_TERRAIN) : (wallMat != MAT_NONE ? wallMat : floorMat);
		if (role == Role.INSIDE && wallMat == MAT_NONE) {
			// The ground under a surface: soil under grass for a few blocks, then stone.
			String top = surfaceKind(floorMat);
			return depth <= 3 && (top.equals("grass") || top.equals("dirt")) ? "minecraft:dirt" : depth <= 1 && top.equals("sand") ? "minecraft:sand" : "minecraft:stone";
		}
		return blockForMaterial(mat, role == Role.TOP && (floorMat != MAT_NONE || wallMat == MAT_NONE));
	}

	/** A coarse kind of the material's surface: road, pavement, grass, dirt, sand, ... (for the ground under it). */
	static String surfaceKind(int mat) {
		String name = mat >= 0 && mat < GTA_MATERIALS.length ? GTA_MATERIALS[mat] : "";
		if (name.startsWith("GRASS") || name.equals("SHORT_GRASS") || name.equals("BUSHES") || name.equals("FLOWERS") || name.equals("LEAVES_PILE")) {
			return "grass";
		}
		if (name.equals("DIRT_DRY") || name.equals("MUD_SOFT") || name.equals("TWIGS") || name.equals("BARK_CHIPPING") || name.equals("CLAY_COURT")) {
			return "dirt";
		}
		if (name.equals("SAND")) {
			return "sand";
		}
		return "other";
	}

	/** The block for a material; {@code floor}: it's what you stand on (else a wall, a ceiling). */
	public static String blockForMaterial(int mat, boolean floor) {
		if (mat >= MAT_GTA_LIMIT || mat < 0 || mat >= GTA_MATERIALS.length) {
			return switch (mat) {
				case MAT_TERRAIN -> "minecraft:smooth_stone";
				case MAT_FLOOR -> "minecraft:light_gray_concrete";
				case MAT_CEILING -> "minecraft:stone_bricks";
				default -> floor ? "minecraft:light_gray_concrete" : "minecraft:stone_bricks";
			};
		}
		String name = GTA_MATERIALS[mat];
		switch (name) {
			case "TARMAC", "PAINTED_ROAD", "CONCRETE_FREEWAY", "RUMBLE_STRIP", "POTHOLE", "POTHOLE_PUDDLE", "PUDDLES":
				return "minecraft:gray_concrete";
			case "CONCRETE":
				return floor ? "minecraft:smooth_stone" : "minecraft:light_gray_concrete";
			case "PAVING_SLABS", "CLAY_COURT":
				return "minecraft:smooth_stone";
			case "PLASTER_BOARD":
				return "minecraft:white_concrete";
			case "BREEZE_BLOCK":
				return "minecraft:stone_bricks";
			case "MOSSY_ROCK":
				return "minecraft:mossy_cobblestone";
			case "ROCK", "STONE":
				return "minecraft:stone";
			case "BRICK_COBBLE":
				return floor ? "minecraft:cobblestone" : "minecraft:bricks";
			case "BRICK_WALL":
				return "minecraft:bricks";
			case "MARBLE":
				return "minecraft:smooth_quartz";
			case "CERAMICS":
				return "minecraft:white_terracotta";
			case "ROOFTILE":
				return "minecraft:brown_terracotta";
			case "RAIL_SLEEPER":
				return "minecraft:spruce_planks";
			case "LAMINATE", "POLISHED_COURT":
				return "minecraft:birch_planks";
			case "GRAVEL", "RAILWAY_GRAVEL":
				return "minecraft:gravel";
			case "SAND":
				return "minecraft:sand";
			case "DIRT_DRY", "TWIGS", "BARK_CHIPPING":
				return floor ? "minecraft:coarse_dirt" : "minecraft:dirt";
			case "MUD_SOFT":
				return "minecraft:mud";
			case "GRASS", "SHORT_GRASS", "GRASS_PATCHY", "GRASS_LONG", "FLOWERS", "LEAVES_PILE":
				return floor ? "minecraft:grass_block" : "minecraft:dirt";
			case "BUSHES":
				return "minecraft:oak_leaves";
			case "TREE_BARK_DARK":
				return "minecraft:dark_oak_log";
			case "TREE_BARK_LIGHT":
				return "minecraft:birch_log";
			case "TREE_BARK_MED":
				return "minecraft:oak_log";
			case "METAL_GRILL", "METAL_LATTICE", "METAL_RAILING", "HOLLOW_METAL_RAIL", "BILLBOARD_FRAME":
				return "minecraft:iron_bars";
			case "HOLLOW_RUST_METAL", "SOLID_RUST_METAL":
				return "minecraft:brown_terracotta";
			case "LEAD_ROOFING", "ROOFING_FELT":
				return "minecraft:gray_concrete";
			case "GLASS_BRICK", "GLASS_MEDIUM", "GLASS_STRONG", "GLASS_WEAK", "PERSPEX", "SKYLIGHTS", "EMISSIVE_GLASS", "GLASS_STRONG_SHOOT_THRU",
				"WINDSCREEN", "TVSCREEN", "VIDEOWALL":
				return "minecraft:glass";
			case "CORRUGATED_PLASTIC", "FIBREGLASS", "PLASTIC", "TARPAULIN", "HOLLOW_PLASTIC", "HOLLOW_FIBREGLASS", "EMISSIVE_PLASTIC":
				return "minecraft:white_terracotta";
			case "LINOLEUM":
				return "minecraft:smooth_stone";
			case "CARPET", "FABRIC_CLOTH", "RUG", "CARPET_FABRIC":
				return "minecraft:red_wool";
			case "RUBBER":
				return "minecraft:black_concrete";
			case "PAPER", "CARDBOARD", "HOLLOW_CARDBOARD":
				return "minecraft:birch_planks";
			case "WATER":
				return "minecraft:water";
			default:
				break;
		}
		if (name.startsWith("WOOD") || name.equals("HOLLOW_WOOD") || name.equals("TREADPLATE_ON_WOOD")) {
			return "minecraft:oak_planks";
		}
		if (name.contains("METAL") || name.startsWith("AIRCON") || name.equals("CORRUGATED_IRON") || name.equals("ELECTRICITY_BOX")
			|| name.startsWith("RAIL_") || name.equals("ROLLER_DOOR")) {
			return "minecraft:iron_block";
		}
		return floor ? "minecraft:light_gray_concrete" : "minecraft:stone_bricks";
	}

	/** Every block id the city can be built of (CityPlan adds water and its gravel bed). */
	public static java.util.Set<String> allBlockIds() {
		java.util.Set<String> ids = new java.util.TreeSet<>();
		for (int mat = 0; mat < 256; mat++) {
			ids.add(blockForMaterial(mat, true));
			ids.add(blockForMaterial(mat, false));
		}
		for (String ground : new String[] { "minecraft:dirt", "minecraft:stone", "minecraft:sand", "minecraft:water", "minecraft:gravel" }) {
			ids.add(ground);
		}
		return ids;
	}

	// ---- material votes from triangles ----------------------------------------------------------------

	/**
	 * Which materials the triangles through an 8x8x8 region bring to each of its blocks: points every
	 * 0.4 block over each triangle vote for the block under them (a floor: the block whose top it
	 * rounds to; a wall or a ceiling: the block it's in). Each block keeps its most voted floor and
	 * wall material.
	 */
	public static final class Votes {
		private static final int SLOTS = 3;
		private final int ox, oy, oz;
		// per block and kind (floor 0, wall 1): SLOTS (material, votes) pairs
		private final int[] mats = new int[512 * 2 * SLOTS];
		private final int[] counts = new int[512 * 2 * SLOTS];

		public Votes(int originX, int originY, int originZ) {
			this.ox = originX;
			this.oy = originY;
			this.oz = originZ;
		}

		/**
		 * One triangle (MC space). {@code mat}: its material code; {@code up}: the normal's y (unit; >= 0.7
		 * a floor, <= -0.7 a ceiling, else a wall).
		 */
		public void triangle(double ax, double ay, double az, double bx, double by, double bz, double cx, double cy, double cz, double up, int mat) {
			double e1 = Math.sqrt(sq(bx - ax) + sq(by - ay) + sq(bz - az));
			double e2 = Math.sqrt(sq(cx - ax) + sq(cy - ay) + sq(cz - az));
			double e3 = Math.sqrt(sq(cx - bx) + sq(cy - by) + sq(cz - bz));
			int n = (int) Math.min(40, Math.max(1, Math.ceil(Math.max(e1, Math.max(e2, e3)) / 0.4)));
			boolean floor = up >= 0.7;
			int kind = floor ? 0 : 1;
			for (int i = 0; i <= n; i++) {
				for (int j = 0; j <= n - i; j++) {
					double u = (double) i / n, v = (double) j / n;
					double x = ax + (bx - ax) * u + (cx - ax) * v;
					double y = ay + (by - ay) * u + (cy - ay) * v;
					double z = az + (bz - az) * u + (cz - az) * v;
					int bxI = (int) Math.floor(x) - this.ox;
					int byI = (floor ? (int) Math.round(y) - 1 : (int) Math.floor(y + 0.01)) - this.oy;
					int bzI = (int) Math.floor(z) - this.oz;
					if (bxI < 0 || bxI > 7 || byI < 0 || byI > 7 || bzI < 0 || bzI > 7) {
						continue;
					}
					vote(bxI + bzI * 8 + byI * 64, kind, mat);
				}
			}
		}

		private void vote(int block, int kind, int mat) {
			int base = (block * 2 + kind) * SLOTS;
			int weakest = base;
			for (int s = base; s < base + SLOTS; s++) {
				if (this.counts[s] > 0 && this.mats[s] == mat) {
					this.counts[s]++;
					return;
				}
				if (this.counts[s] < this.counts[weakest]) {
					weakest = s;
				}
			}
			if (this.counts[weakest] == 0) {
				this.mats[weakest] = mat;
				this.counts[weakest] = 1;
			} else {
				this.counts[weakest]--; // a full table: the least voted one loses a vote (and its slot at 0)
			}
		}

		/** The most voted material of this block (index x + 8z + 64y) and kind (0 floor, 1 wall), or MAT_NONE. */
		public int best(int block, int kind) {
			int base = (block * 2 + kind) * SLOTS, best = MAT_NONE, votes = 0;
			for (int s = base; s < base + SLOTS; s++) {
				if (this.counts[s] > votes) {
					votes = this.counts[s];
					best = this.mats[s];
				}
			}
			return best;
		}

		private static double sq(double v) {
			return v * v;
		}
	}
}
