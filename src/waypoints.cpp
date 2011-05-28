// 3D World - OpenGL CS184 Computer Graphics Project
// by Frank Gennari
// 4/13/02

#include "3DWorld.h"
#include "mesh.h"
#include "physics_objects.h"
#include "player_state.h"
#include <queue>


bool const SHOW_WAYPOINTS      = 0;
bool const SHOW_WAYPOINT_EDGES = 0;
int const WP_RESET_FRAMES      = 100; // Note: in frames, not ticks, fix?
int const WP_RECENT_FRAMES     = 200;
float const MAX_FALL_DIST_MULT = 20.0;

bool has_user_placed(0), has_item_placed(0), has_wpt_goal(0);
vector<waypoint_t> waypoints;

extern bool use_waypoints;
extern int DISABLE_WATER, camera_change, frame_counter, num_smileys, num_groups;
extern float temperature, zmin, tfticks, water_plane_z;
extern int coll_id[];
extern obj_group obj_groups[];
extern obj_type object_types[];
extern dwobject def_objects[];
extern vector<coll_obj> coll_objects;


// ********** waypt_used_set **********


void waypt_used_set::clear() {

	used.clear();
	last_wp    = 0;
	last_frame = 0;
}

void waypt_used_set::insert(unsigned wp) { // called when a waypoint has been reached

	last_wp    = wp;
	last_frame = frame_counter;
	used[wp]   = frame_counter;
}

bool waypt_used_set::is_valid(unsigned wp) { // called to determine whether or not a waypoint is valid based on when it was last used

	if (last_frame > 0) {
		if ((frame_counter - last_frame) >= WP_RECENT_FRAMES) {
			clear(); // last_wp has expired, so all of the others must have expired as well
			return 1;
		}
		if (wp == last_wp) return 0; // too recent (lasts used)
	}
	map<unsigned, int>::iterator it(used.find(wp));
	if (it == used.end()) return 1; // new waypoint
	if ((frame_counter - it->second) < WP_RESET_FRAMES) return 0; // too recent
	used.erase(it); // lazy update - remove the waypoint when found to be expired
	return 1;
}


// ********** waypoint_t **********


waypoint_t::waypoint_t(point const &p, bool up, bool i, bool g, bool t)
	: user_placed(up), placed_item(i), goal(g), temp(t), visited(0),
	item_group(-1), item_ix(-1), came_from(-1), g_score(0), h_score(0), f_score(0), pos(p)
{
	clear();
}


void waypoint_t::mark_visited_by_smiley(unsigned const smiley_id) {

	last_smiley_time = tfticks; // for now, we don't care which smiley
	visited = 1;
}


float waypoint_t::get_time_since_last_visited(unsigned const smiley_id) const {

	float const delta_t(tfticks - last_smiley_time); // for now, we don't care which smiley
	assert(delta_t >= 0.0);
	return delta_t;
}


void waypoint_t::clear() {

	last_smiley_time = tfticks;
	next_wpts.clear();
	prev_wpts.clear();
	visible_wpts.clear();
	visited = 0;
}


wpt_goal::wpt_goal(int m, unsigned w, point const &p) : mode(m), wpt(w), pos(p) {

	switch (mode) {
	case 0: case 1: case 2: case 3: case 5: case 6: break; // nothing
	case 4: assert(wpt < waypoints.size()); break;
	case 7: assert(is_over_mesh(pos));      break;
	default: assert(0);
	}
}


bool wpt_goal::is_reachable() const {

	if (mode == 0 || waypoints.empty()) return 0;
	if (mode == 1 && !has_user_placed)  return 0;
	if (mode == 2 && !has_item_placed)  return 0;
	if (mode == 3 && !has_wpt_goal)     return 0;
	return 1;
}


// ********** waypoint_builder **********


bool check_step_dz(point &cur, point const &lpos, float radius) {

	float zvel(0.0);
	int const ret(set_true_obj_height(cur, lpos, C_STEP_HEIGHT, zvel, WAYPOINT, -2, 0, 0, 1));
	if (ret == 3)                                      return 0; // stuck
	if ((cur.z - lpos.z) > C_STEP_HEIGHT*radius)       return 0; // too high of a step
	if ((cur.z - lpos.z) < -MAX_FALL_DIST_MULT*radius) return 0; // too far  of a drop
	return 1;
}


class waypoint_builder {

	float radius;

	bool is_waypoint_valid(point pos, int coll_id) const {
		if (pos.z < zmin || !is_over_mesh(pos)) return 0;
		player_clip_to_scene(pos); // make sure players can reach this waypoint
		float const mesh_zval(interpolate_mesh_zval(pos.x, pos.y, 0.0, 0, 0));
		if (pos.z - radius < mesh_zval) return 0; // bottom of smiley is under the mesh - should use a mesh waypoint here
		return check_cobj_placement(point(pos), coll_id);
	}

	void add_if_valid(point const &pos, int coll_id) {
		if (is_waypoint_valid(pos, coll_id)) waypoints.push_back(waypoint_t(pos, 0));
	}

	void add_waypoint_rect(float x1, float y1, float x2, float y2, float z, int coll_id) {
		if (min(x2-x1, y2-y1) < radius) return; // too small to stand on
		point const center(0.5*(x1+x2), 0.5*(y1+y2), z+radius);
		add_if_valid(center, coll_id);
		// FIXME: try more points?
	}

	void add_waypoint_circle(point const &p, float r, int coll_id) {
		if (r < radius) return; // too small to stand on
		add_if_valid(p + point(0.0, 0.0, radius), coll_id);
	}

	void add_waypoint_triangle(point const &p1, point const &p2, point const &p3, int coll_id) {
		if (dist_less_than(p1, p2, 2*radius) || dist_less_than(p2, p3, 2*radius) || dist_less_than(p3, p1, 2*radius)) return; // too small to stand on
		point const center(((p1 + p2 + p3) / 3.0) + point(0.0, 0.0, radius));
		add_if_valid(center, coll_id);
		// could try more points (corners?)
	}

	void add_waypoint_poly(point const *const points, unsigned npoints, vector3d const &norm, int coll_id) {
		assert(npoints == 3 || npoints == 4);
		if (fabs(norm.z) < 0.5) return; // need a mostly vertical polygon to stand on
		add_waypoint_triangle(points[0], points[1], points[2], coll_id);
		if (npoints == 4) add_waypoint_triangle(points[0], points[2], points[3], coll_id); // quad only
	}

public:
	waypoint_builder(void) : radius(object_types[WAYPOINT].radius) {}
	
	void add_cobj_waypoints(vector<coll_obj> const &cobjs) {
		int const cc(camera_change);
		camera_change = 0; // messes up collision detection code
		unsigned const num_waypoints(waypoints.size());

		for (vector<coll_obj>::const_iterator i = cobjs.begin(); i != cobjs.end(); ++i) {
			if (i->status != COLL_STATIC || i->platform_id >= 0) continue; // only static objects (not platforms) - use i->truly_static()?

			switch (i->type) {
			case COLL_CUBE: // can stand on the top
				add_waypoint_rect(i->d[0][0], i->d[1][0], i->d[0][1], i->d[1][1], i->d[2][1], i->id); // top rect
				break;

			case COLL_CYLINDER: // can stand on the top
				if (i->points[0].z > i->points[1].z) {
					add_waypoint_circle(i->points[0], i->radius,  i->id);
				}
				else {
					add_waypoint_circle(i->points[1], i->radius2, i->id);
				}
				break;

			case COLL_POLYGON:
				assert(i->npoints == 3 || i->npoints == 4); // triangle or quad
				
				if (i->thickness > MIN_POLY_THICK2) { // extruded polygon
					vector<vector<point> > const &pts(thick_poly_to_sides(i->points, i->npoints, i->norm, i->thickness));

					for (unsigned j = 0; j < pts.size(); ++j) {
						add_waypoint_poly(&pts[j].front(), pts[j].size(), get_poly_norm(&pts[j].front()), i->id);
					}
				}
				else {
					add_waypoint_poly(i->points, i->npoints, i->norm, i->id);
				}
				break;

			case COLL_SPHERE:       break; // not supported (can't stand on)
			case COLL_CYLINDER_ROT: break; // not supported (can't stand on)
			default: assert(0);
			}
		}
		cout << "Added " << (waypoints.size() - num_waypoints) << " cobj waypoints" << endl;
		camera_change = cc;
	}

	void add_mesh_waypoints() {
		unsigned const mesh_skip_dist(16);
		unsigned const num_waypoints(waypoints.size());

		for (int yy = 0; yy <= MESH_Y_SIZE; yy += mesh_skip_dist) {
			for (int xx = 0; xx <= MESH_X_SIZE; xx += mesh_skip_dist) {
				int const x(max(1, min(MESH_X_SIZE-2, xx))), y(max(1, min(MESH_Y_SIZE-2, yy)));
				if (is_mesh_disabled(x, y)) continue; // mesh disabled
				float zval(mesh_height[y][x]);
			
				if (!DISABLE_WATER && has_water(x, y) && zval < water_plane_z) { // underwater
					if (temperature <= W_FREEZE_POINT) { // ice
						zval = water_matrix[y][x]; // walk on ice
					}
					else { // water
						continue; // don't go underwater
					}
				}
				// *** WRITE - more filtering ***
				point const pos(get_xval(x), get_yval(y), (zval + radius));
				add_if_valid(pos, -1);
			}
		}
		cout << "Added " << (waypoints.size() - num_waypoints) << " terrain waypoints" << endl;
	}

	void add_object_waypoints() {
		unsigned const num_waypoints(waypoints.size());

		for (int i = 0; i < num_groups; ++i) {
			vector<predef_obj> const &objs(obj_groups[i].get_predef_objs());

			for (unsigned j = 0; j < objs.size(); ++j) {
				waypoint_t w(objs[j].pos, 0, 1);
				w.item_group = i;
				w.item_ix    = j;
				waypoints.push_back(w);
				has_item_placed = 1;
			}
		}
		cout << "Added " << (waypoints.size() - num_waypoints) << " object placement waypoints" << endl;
	}

	unsigned add_temp_waypoint(point const &pos, bool connect_in, bool connect_out, bool goal) {
		unsigned const ix(waypoints.size());
		waypoints.push_back(waypoint_t(pos, 0, 0, goal, 1));
		if (connect_in ) connect_waypoints(0,  ix,    ix, ix+1, 0); // from existing waypoints to new waypoint
		if (connect_out) connect_waypoints(ix, ix+1,  0,  ix,   0); // from new waypoint to existing waypoints
		return ix;
	}

	void disconnect_waypoint(unsigned ix) {
		assert(ix < waypoints.size());
		waypoint_t &w(waypoints[ix]);
		
		for (waypt_adj_vect::const_iterator i = w.prev_wpts.begin(); i != w.prev_wpts.end(); ++i) {
			assert(*i < waypoints.size());
			assert(!waypoints[*i].next_wpts.empty() && waypoints[*i].next_wpts.back() == ix);
			waypoints[*i].next_wpts.pop_back();
		}
		for (waypt_adj_vect::const_iterator i = w.next_wpts.begin(); i != w.next_wpts.end(); ++i) {
			assert(*i < waypoints.size());
			assert(!waypoints[*i].prev_wpts.empty() && waypoints[*i].prev_wpts.back() == ix);
			waypoints[*i].prev_wpts.pop_back();
		}
		w.clear();
	}

	void remove_last_waypoint() {
		assert(!waypoints.empty());
		assert(waypoints.back().temp); // too strict?
		disconnect_waypoint(waypoints.size() - 1);
		waypoints.pop_back();
	}

	void add_edge(unsigned from, unsigned to) {
		assert(from < waypoints.size());
		assert(to   < waypoints.size());
		waypoints[from].next_wpts.push_back(to  );
		waypoints[to  ].prev_wpts.push_back(from);
	}

	void connect_all_waypoints() {
		connect_waypoints(0, waypoints.size(), 0, waypoints.size(), 1);
	}

	void connect_waypoints(unsigned from_start, unsigned from_end, unsigned to_start, unsigned to_end, bool verbose) {
		unsigned visible(0), cand_edges(0), num_edges(0), tot_steps(0);
		unsigned const num_waypoints(waypoints.size());
		int cindex(-1);
		vector<pair<float, unsigned> > cands;
		//sort(waypoints.begin(), waypoints.end());

		for (unsigned i = from_start; i < from_end; ++i) {
			point const start(waypoints[i].pos);
			cands.resize(0);

			for (unsigned j = to_start; j < to_end; ++j) {
				point const end(waypoints[j].pos);

				if (cindex >= 0) {
					assert((unsigned)cindex < coll_objects.size());
					if (coll_objects[cindex].line_intersect(start, end)) continue; // hit last cobj
				}
				if (i == j || check_coll_line(start, end, cindex, -1, 1, 0)) continue;
				waypoints[i].visible_wpts.push_back(j);
				cands.push_back(make_pair(p2p_dist_sq(start, end), j));
				++visible;
			}
			sort(cands.begin(), cands.end()); // closest to furthest
			waypt_adj_vect const &next(waypoints[i].next_wpts);

			for (unsigned j = 0; j < cands.size(); ++j) {
				unsigned const k(cands[j].second);
				point const end(waypoints[k].pos);
				vector3d const dir(end - start), dir_xy(vector3d(dir.x, dir.y, 0.0).get_norm());
				bool colinear(0), redundant(0);

				for (unsigned l = 0; l < next.size() && !colinear; ++l) {
					assert(next[l] < waypoints.size());
					if (next[l] < to_start || next[l] >= to_end) continue; // no in the target range
					vector3d const dir2(waypoints[next[l]].pos - start), dir_xy2(vector3d(dir2.x, dir2.y, 0.0).get_norm());
					colinear = (dot_product(dir_xy, dir_xy2) > 0.99);
				}
				if (colinear) continue;

				for (unsigned l = 0; l < next.size() && !redundant; ++l) {
					waypt_adj_vect const &next_next(waypoints[next[l]].next_wpts);
					point const &wl(waypoints[next[l]].pos);

					for (unsigned m = 0; m < next_next.size() && !redundant; ++m) {
						point const &wm(waypoints[next_next[m]].pos);
						redundant = (next_next[m] == k && (p2p_dist(start, wl) + p2p_dist(wl, wm) < 1.02*p2p_dist(start, wm)));
					}
				}
				if (redundant) continue;

				if (is_point_reachable(start, end, tot_steps)) {
					add_edge(i, k);
					++num_edges;
				}
				++cand_edges;
			}
		}
		if (verbose) cout << "vis edges: " << visible << ", cand edges: " << cand_edges << ", true edges: " << num_edges << ", tot steps: " << tot_steps << endl;
#if 0
		RESET_TIME; // performance test
		for (unsigned iter = 0; iter < 10; ++iter) {
			for (unsigned from = 0; from < waypoints.size(); from += 10) {
				for (unsigned to = 0; to < waypoints.size(); to += 10) {
					find_optimal_next_waypoint(from, wpt_goal(4, to, all_zeros));
				}
			}
		}
		PRINT_TIME("Waypoint Perf");
#endif
	}


	bool check_cobj_placement(point &pos, int coll_id) const {
		dwobject obj(def_objects[WAYPOINT]); // create a fake temporary smiley object
		obj.pos     = pos;
		obj.coll_id = coll_id; // ignore collisions with the current object
		bool const ret(!obj.check_vert_collision(0, 0, 0, NULL, all_zeros, 1)); // return true if no collision (skip dynamic objects)
		pos = obj.pos;
		return ret;
	}

	bool is_point_reachable(point const &start, point const &end, unsigned &tot_steps) const {
		vector3d const dir(end - start);
		float const step_size(0.25*radius);
		float const dmag_inv(1.0/dir.xy_mag());
		point cur(start);

		while (!dist_less_than(cur, end, 0.8*radius)) {
			vector3d const delta((end - cur).get_norm());
			point lpos(cur);
			cur += delta*step_size;
			if (!check_step_dz(cur, lpos, radius))               return 0;
			check_cobj_placement(cur, -1);
			if (dot_product_ptv(delta, cur, lpos) < 0.01*radius) return 0; // not making progress (too strict? local drops in z?)
			float const d(fabs((end.x - start.x)*(start.y - cur.y) - (end.y - start.y)*(start.x - cur.x))*dmag_inv); // point-line dist
			if (d > 2.0*radius)                                  return 0; // path deviation too long
			++tot_steps;
			//waypoints.push_back(waypoint_t(cur, 1)); // testing
		}
		return 1; // success
	}

	// is check_visible==1, only consider visible and reachable (at least one incoming edge) waypoints
	int find_closest_waypoint(point const &pos, bool check_visible) const {
		int closest(-1), cindex(-1);
		float closest_dsq(0.0);

		// inefficient to iterate, might need acceleration structure
		for (unsigned i = 0; i < waypoints.size(); ++i) {
			float const dist_sq(p2p_dist_sq(pos, waypoints[i].pos));

			if (closest < 0 || dist_sq < closest_dsq) {
				if (check_visible && (waypoints[i].unreachable() ||
					check_coll_line(pos, waypoints[i].pos, cindex, -1, 1, 0))) continue; // not visible/reachable
				closest_dsq = dist_sq;
				closest     = i;
			}
		}
		return closest;
	}
};


// ********** waypoint_search **********


unsigned const PATH_CACHE_SIZE = 0;//(1 << 16);


struct waypoint_cache {

	vector<unsigned> open, closed; // tentative/already evaulated nodes
	unsigned call_ix; // incremented each run_a_star() call

	struct path_cache_entry { // unused, but may be useful
		unsigned short from, to, next;
		float dist;
		path_cache_entry() : from(0), to(0), next(0), dist(0.0) {}
	};
	vector<path_cache_entry> path_cache;

	waypoint_cache() : call_ix(0) {
		path_cache.resize(PATH_CACHE_SIZE);
	}
	path_cache_entry &cache_lookup(unsigned from, unsigned to) {
		return path_cache[(from + (to << 16)) & (PATH_CACHE_SIZE-1)];
	}
};

waypoint_cache global_wpt_cache;


class waypoint_search {

	wpt_goal goal;
	waypoint_builder wb;
	waypoint_cache &wc;

	float get_h_dist(unsigned cur) const {
		return ((goal.mode >= 4) ? p2p_dist(waypoints[cur].pos, goal.pos) : 0.0);
	}
	bool is_goal(unsigned cur) const {
		waypoint_t const &w(waypoints[cur]);
		if (goal.mode == 1) return w.user_placed;     // user waypoint
		if (goal.mode == 3) return w.goal;            // goal waypoint
		if (goal.mode >= 4) return (cur == goal.wpt); // goal position or specific waypoint

		if (goal.mode == 2 && waypoints[cur].placed_item) { // placed item waypoint
			if (w.item_group >= 0) { // check if item is present
				assert(w.item_group < NUM_TOT_OBJS);
				obj_group const &objg(obj_groups[w.item_group]);
				if (!objg.is_enabled()) return 0;
				vector<predef_obj> const &objs(objg.get_predef_objs());
				assert(w.item_ix >= 0 && (unsigned)w.item_ix < objs.size());
				return (objs[w.item_ix].obj_used >= 0); // in use
			}
			return 1;
		}
		return 0;
	}
	void reconstruct_path(unsigned cur, vector<unsigned> &path) {
		assert(cur >= 0 && cur < waypoints.size());
		if (waypoints[cur].came_from >= 0) reconstruct_path(waypoints[cur].came_from, path);
		path.push_back(cur);
	}

public:
	waypoint_search(wpt_goal const &goal_, waypoint_cache &wc_) : goal(goal_), wc(wc_) {}

	// returns min distance to goal following connected waypoints along path
	float run_a_star(vector<pair<unsigned, float> > const &start, vector<unsigned> &path) {
		if (!goal.is_reachable()) return 0.0; // nothing to do
		assert(path.empty());
		bool const orig_has_wpt_goal(has_wpt_goal);
		if (goal.mode == 4) goal.pos = waypoints[goal.wpt].pos; // specific waypoint
		if (goal.mode == 5) goal.wpt = wb.find_closest_waypoint(goal.pos, 0);
		if (goal.mode == 6) goal.wpt = wb.find_closest_waypoint(goal.pos, 1);
		if (goal.mode == 7) goal.wpt = wb.add_temp_waypoint(goal.pos, 1, 1, 1); // goal position - add temp waypoint
		if (goal.mode == 7) has_wpt_goal = 1;
		//cout << "start: " << start.size() << ", goal: mode: " << goal.mode << ", pos: "; goal.pos.print(); cout << ", wpt: " << goal.wpt << endl;
		
		std::priority_queue<pair<float, unsigned> > open_queue;
		wc.open.resize(waypoints.size(), 0); // already resized after the first call
		wc.closed.resize(waypoints.size(), 0);
		++wc.call_ix;

		for (vector<pair<unsigned, float> >::const_iterator i = start.begin(); i != start.end(); ++i) {
			unsigned const ix(i->first);
			assert(ix < waypoints.size());
			waypoint_t &w(waypoints[ix]);
			w.g_score   = i->second; // Cost from start along best known path.
			w.h_score   = get_h_dist(ix);
			w.f_score   = w.h_score; // Estimated total cost from start to goal through current.
			w.came_from = -1;

			if (is_goal(ix)) { // already at the goal
				path.push_back(ix);
				return w.f_score;
			}
			wc.open[ix] = wc.call_ix;
			open_queue.push(make_pair(-w.f_score, ix));
		}
		if (goal.mode >= 4 && waypoints[goal.wpt].unreachable()) return 0.0; // goal has no incoming edges - unreachable
		float min_dist(0.0);

		while (!open_queue.empty()) {
			unsigned const cur(open_queue.top().second);
			open_queue.pop();
			if (wc.closed[cur] == wc.call_ix) continue; // already closed (duplicate)
			waypoint_t const &cw(waypoints[cur]);

			if (is_goal(cur)) {
				reconstruct_path(cur, path);
				min_dist = cw.f_score;
				break; // we're done
			}
			assert(wc.closed[cur] != wc.call_ix);
			wc.closed[cur] = wc.call_ix;
			wc.open[cur]   = 0;

			for (waypt_adj_vect::const_iterator i = cw.next_wpts.begin(); i != cw.next_wpts.end(); ++i) {
				if (wc.closed[*i] == wc.call_ix) continue; // already closed (duplicate)
				assert(*i < waypoints.size());
				waypoint_t &wn(waypoints[*i]);
				float const new_g_score(cw.g_score + p2p_dist(cw.pos, wn.pos));
				bool better(0);

				if (wc.open[*i] != wc.call_ix) {
					wc.open[*i] = wc.call_ix;
					better = 1;
				}
				else if (new_g_score < wn.g_score) {
					better = 1;
				}
				if (better) {
					wn.came_from = cur;
					wn.g_score   = new_g_score;
					wn.h_score   = get_h_dist(*i);
					wn.f_score   = wn.g_score + wn.h_score;
					open_queue.push(make_pair(-wn.f_score, *i));
				}
			} // for i
		}
		if (goal.mode == 7) {
			wb.remove_last_waypoint(); // goal position - remove temp waypoint
			has_wpt_goal = orig_has_wpt_goal;
		}
		return min_dist;
	}
};


// ********** waypoint top level code **********


void create_waypoints(vector<user_waypt_t> const &user_waypoints) {

	RESET_TIME;
	waypoints.clear();
	has_user_placed = (!user_waypoints.empty());
	has_item_placed = 0;
	has_wpt_goal    = 0;
	
	for (vector<user_waypt_t>::const_iterator i = user_waypoints.begin(); i != user_waypoints.end(); ++i) {
		waypoints.push_back(waypoint_t(i->pos, 1, 0, (i->type == 1))); // goal is type 1
		if (waypoints.back().goal) has_wpt_goal = 1;
	}
	waypoint_builder wb;

	if (use_waypoints) {
		wb.add_cobj_waypoints(coll_objects);
		wb.add_mesh_waypoints();
		wb.add_object_waypoints();
		PRINT_TIME("  Waypoint Generation");
	}
	wb.connect_all_waypoints();
	PRINT_TIME("  Waypoint Connectivity");
	cout << "Waypoints: " << waypoints.size() << endl;
}


// find the optimal next waypoint when already on a waypoint path
int find_optimal_next_waypoint(unsigned cur, wpt_goal const &goal) {

	if (!goal.is_reachable()) return -1; // nothing to do
	//RESET_TIME;
	vector<unsigned> path;
	waypoint_search ws(goal, global_wpt_cache);
	vector<pair<unsigned, float> > start;
	start.push_back(make_pair(cur, 0.0));
	ws.run_a_star(start, path);
	//PRINT_TIME("A Star");
	if (path.empty())     return -1; // no path to goal
	assert(path[0] == cur);
	if (path.size() == 1) return cur; // already at goal
	return path[1];
}


// find the optimal next waypoint when not on a waypoint path (using visible waypoints as candidates)
void find_optimal_waypoint(point const &pos, vector<od_data> &oddatav, wpt_goal const &goal) {

	if (oddatav.empty() || !goal.is_reachable()) return; // nothing to do
	//RESET_TIME;
	vector<pair<float, unsigned> > cands(oddatav.size());
	vector<pair<unsigned, float> > start;
	waypoint_builder wb;
	float min_dist(0.0);

	for (unsigned i = 0; i < oddatav.size(); ++i) {
		cands[i] = make_pair(p2p_dist(pos, waypoints[oddatav[i].id].pos), oddatav[i].id);
	}
	sort(cands.begin(), cands.end());

	for (unsigned i = 0; i < cands.size(); ++i) {
		unsigned const id(cands[i].second);
		point const &wpos(waypoints[id].pos);
		float const dist(cands[i].first);
		if (min_dist > 0.0 && dist > 2.0*min_dist) break; // we're done
		int cindex(-1);
		unsigned tot_steps(0);

		if (!check_coll_line(pos, wpos, cindex, -1, 1, 0) && wb.is_point_reachable(pos, wpos, tot_steps)) {
			min_dist = (start.empty() ? dist : min(dist, min_dist));
			start.push_back(make_pair(id, dist));
		}
	}
	waypoint_search ws(goal, global_wpt_cache);
	vector<unsigned> path;
	ws.run_a_star(start, path);
	//PRINT_TIME("Find Optimal Waypoint");
	//cout << "query size: " << oddatav.size() << ", start size: " << start.size() << ", path length: " << path.size() << endl;
	if (path.empty()) return; // no path found, nothing to do
	unsigned const best(path[0]);

	for (unsigned i = 0; i < oddatav.size(); ++i) {
		oddatav[i].dist = ((oddatav[i].id == best) ? 1.0 : 1000.0); // large/small distance
	}
}


// return true if coll_detect(pos) is closer to pos than opos
bool can_make_progress(point const &pos, point const &opos) {

	waypoint_builder wb;
	point test_pos(pos);
	wb.check_cobj_placement(test_pos, -1);
	return (p2p_dist_xy_sq(test_pos, pos) < p2p_dist_xy_sq(test_pos, opos)); // ignore z
}


// return true if the path from start to end is traversable by a smiley/player
bool is_valid_path(point const &start, point const &end) {

	waypoint_builder wb;
	unsigned tot_steps(0); // unused
	return wb.is_point_reachable(start, end, tot_steps);
}


void shift_waypoints(vector3d const &vd) {

	for (unsigned i = 0; i < waypoints.size(); ++i) {
		waypoints[i].pos += vd;
	}
}


void draw_waypoints() {

	if (!SHOW_WAYPOINTS) return;
	pt_line_drawer pld;

	for (vector<waypoint_t>::const_iterator i = waypoints.begin(); i != waypoints.end(); ++i) {
		unsigned const wix(i - waypoints.begin());
		if      (i->visited)     set_color(ORANGE);
		else if (i->goal)        set_color(RED);
		else if (i->user_placed) set_color(YELLOW);
		else if (i->placed_item) set_color(PURPLE);
		else                     set_color(WHITE);
		draw_sphere_at(i->pos, 0.25*object_types[WAYPOINT].radius, N_SPHERE_DIV/2);
		if (!SHOW_WAYPOINT_EDGES) continue;

		for (waypt_adj_vect::const_iterator j = i->next_wpts.begin(); j != i->next_wpts.end(); ++j) {
			assert(*j < waypoints.size());
			assert(*j != wix);
			waypoint_t const &w(waypoints[*j]);
			bool bidir(0);

			for (waypt_adj_vect::const_iterator k = w.next_wpts.begin(); k != w.next_wpts.end() && !bidir; ++k) {
				bidir = (*k == wix);
			}
			if (!bidir || *j < wix) {
				pld.add_line(i->pos, plus_z, (bidir ? YELLOW : WHITE), w.pos, plus_z, (bidir ? YELLOW : ORANGE));
			}
		}
	}
	pld.draw();
}





