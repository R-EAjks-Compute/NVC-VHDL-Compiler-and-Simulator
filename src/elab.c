//
//  Copyright (C) 2011-2025  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "util.h"
#include "array.h"
#include "common.h"
#include "diag.h"
#include "driver.h"
#include "eval.h"
#include "hash.h"
#include "inst.h"
#include "lib.h"
#include "lower.h"
#include "mask.h"
#include "object.h"
#include "option.h"
#include "phase.h"
#include "psl/psl-phase.h"
#include "rt/model.h"
#include "rt/structs.h"
#include "thread.h"
#include "type.h"
#include "vlog/vlog-defs.h"
#include "vlog/vlog-node.h"
#include "vlog/vlog-phase.h"
#include "vlog/vlog-util.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>

#define MAX_DEPTH 127    // Limited by vcode type indexes

typedef A(tree_t) tree_list_t;

typedef struct _elab_ctx elab_ctx_t;
typedef struct _generic_list generic_list_t;

typedef struct _elab_ctx {
   const elab_ctx_t *parent;
   tree_t            out;
   object_t         *root;
   tree_t            inst;
   tree_t            config;
   ident_t           inst_name;     // Current 'INSTANCE_NAME
   ident_t           dotted;
   ident_t           prefix[2];
   lib_t             library;
   hash_t           *generics;
   jit_t            *jit;
   unit_registry_t  *registry;
   mir_context_t    *mir;
   lower_unit_t     *lowered;
   cover_data_t     *cover;
   sdf_file_t       *sdf;
   driver_set_t     *drivers;
   hash_t           *modcache;
   rt_model_t       *model;
   rt_scope_t       *scope;
   unsigned          depth;
} elab_ctx_t;

typedef struct {
   ident_t     search;
   ident_t     chosen;
   timestamp_t mtime;
} lib_search_params_t;

typedef struct {
   tree_t  comp;
   tree_t *result;
} synth_binding_params_t;

typedef struct _generic_list {
   generic_list_t *next;
   ident_t         name;
   char           *value;
} generic_list_t;

typedef struct {
   vcode_unit_t shape;
   tree_t       block;
   tree_t       wrap;
   vlog_node_t  module;
} mod_cache_t;

static void elab_block(tree_t t, const elab_ctx_t *ctx);
static void elab_stmts(tree_t t, const elab_ctx_t *ctx);
static void elab_decls(tree_t t, const elab_ctx_t *ctx);
static void elab_push_scope(tree_t t, elab_ctx_t *ctx);
static void elab_pop_scope(elab_ctx_t *ctx);

static generic_list_t *generic_override = NULL;

static ident_t hpathf(ident_t path, char sep, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char *buf = xvasprintf(fmt, ap);
   va_end(ap);

   // LRM specifies instance path is lowercase
   char *p = buf;
   while (*p != '\0') {
      *p = tolower_iso88591(*p);
      ++p;
   }

   ident_t id = ident_new(buf);
   free(buf);
   return ident_prefix(path, id, sep);
}

static const char *simple_name(const char *full)
{
   // Strip off any library or entity prefix from the parameter
   const char *start = full;
   for (const char *p = full; *p != '\0'; p++) {
      if (*p == '.' || *p == '-')
         start = p + 1;
   }

   return start;
}

static lib_t elab_find_lib(ident_t name, const elab_ctx_t *ctx)
{
   ident_t lib_name = ident_until(name, '.');
   if (lib_name == well_known(W_WORK))
      return ctx->library;
   else
      return lib_require(lib_name);
}

static void elab_find_arch_cb(lib_t lib, ident_t name, int kind, void *context)
{
   lib_search_params_t *params = context;

   ident_t prefix = ident_until(name, '-');

   if (kind != T_ARCH || prefix != params->search)
      return;

   const timestamp_t new_mtime = lib_get_mtime(lib, name);

   if (params->chosen == NULL) {
      params->chosen = name;
      params->mtime  = new_mtime;
   }
   else if (new_mtime > params->mtime) {
      params->chosen = name;
      params->mtime  = new_mtime;
   }
   else if (new_mtime == params->mtime) {
      // Use source file line numbers to break the tie
      tree_t old_unit = lib_get(lib, params->chosen);
      tree_t new_unit = lib_get(lib, name);

      if (old_unit == NULL)
         params->chosen = name;
      else if (new_unit != NULL) {
         const loc_t *old_loc = tree_loc(old_unit);
         const loc_t *new_loc = tree_loc(new_unit);

         if (old_loc->file_ref != new_loc->file_ref)
            warnf("cannot determine which of %s and %s is most recently "
                  "modified", istr(params->chosen), istr(name));
         else if (new_loc->first_line >= old_loc->first_line)
            params->chosen = name;
      }
   }
}

static tree_t elab_pick_arch(const loc_t *loc, tree_t entity,
                             const elab_ctx_t *ctx)
{
   // When an explicit architecture name is not given select the most
   // recently analysed architecture of this entity

   ident_t name = tree_ident(entity);
   lib_t lib = elab_find_lib(name, ctx);
   ident_t search_name =
      ident_prefix(lib_name(lib), ident_rfrom(name, '.'), '.');

   lib_search_params_t params = {
      .search = search_name
   };
   lib_walk_index(lib, elab_find_arch_cb, &params);

   if (params.chosen == NULL)
      fatal_at(loc, "no suitable architecture for %s", istr(search_name));

   return lib_get(lib, params.chosen);
}

static tree_t elab_copy(tree_t t, const elab_ctx_t *ctx)
{
   tree_list_t roots = AINIT;
   switch (tree_kind(t)) {
   case T_ARCH:
      APUSH(roots, tree_primary(t));
      APUSH(roots, t);    // Architecture must be processed last
      break;
   case T_BLOCK_CONFIG:
      {
         tree_t arch = tree_ref(t);
         assert(tree_kind(arch) == T_ARCH);

         APUSH(roots, tree_primary(arch));
         APUSH(roots, arch);
         APUSH(roots, t);
      }
      break;
   default:
      fatal_trace("unexpected %s in elab_copy", tree_kind_str(tree_kind(t)));
   }

   tree_global_flags_t gflags = 0;
   for (int i = 0; i < roots.count; i++)
      gflags |= tree_global_flags(roots.items[i]);

   new_instance(roots.items, roots.count, ctx->dotted, ctx->prefix,
                ARRAY_LEN(ctx->prefix));

   tree_t copy = roots.items[roots.count - 1];
   ACLEAR(roots);

   tree_set_global_flags(copy, gflags);
   return copy;
}

static void elab_subprogram_prefix(tree_t arch, elab_ctx_t *ctx)
{
   // Get the prefix of unit that will need to be rewritten in
   // subprogram names

   assert(tree_kind(arch) == T_ARCH);

   // The order is important here because the architecture name is
   // prefixed with the entity
   ctx->prefix[0] = tree_ident(arch);
   ctx->prefix[1] = tree_ident(tree_primary(arch));
}

static mod_cache_t *elab_cached_module(vlog_node_t mod, const elab_ctx_t *ctx)
{
   assert(is_top_level(mod));

   mod_cache_t *mc = hash_get(ctx->modcache, mod);
   if (mc == NULL) {
      mc = xcalloc(sizeof(mod_cache_t));
      mc->module = mod;
      mc->shape  = vlog_lower(ctx->registry, ctx->mir, mod);

      mc->block = tree_new(T_BLOCK);
      tree_set_loc(mc->block, vlog_loc(mod));
      tree_set_ident(mc->block, vlog_ident(mod));

      vlog_trans(mod, mc->block);

      mc->wrap = tree_new(T_VERILOG);
      tree_set_loc(mc->wrap, vlog_loc(mod));
      tree_set_ident(mc->wrap, vlog_ident(mod));
      tree_set_vlog(mc->wrap, mod);

      hash_put(ctx->modcache, mod, mc);
   }

   return mc;
}

static bool elab_synth_binding_cb(lib_t lib, void *__ctx)
{
   synth_binding_params_t *params = __ctx;

   ident_t name = ident_prefix(lib_name(lib), tree_ident(params->comp), '.');
   *(params->result) = lib_get(lib, name);

   return *(params->result) == NULL;
}

static tree_t elab_to_vhdl(type_t from, type_t to)
{
   static struct {
      const verilog_type_t from_id;
      const ieee_type_t    to_id;
      const char *const    func;
      type_t               from;
      type_t               to;
      tree_t               decl;
   } table[] = {
      { VERILOG_LOGIC, IEEE_STD_LOGIC, "NVC.VERILOG.TO_VHDL(" T_LOGIC ")U" },
      { VERILOG_NET_VALUE, IEEE_STD_LOGIC,
        "NVC.VERILOG.TO_VHDL(" T_NET_VALUE ")U" },
   };

   INIT_ONCE({
         for (int i = 0; i < ARRAY_LEN(table); i++) {
            table[i].from = verilog_type(table[i].from_id);
            table[i].to   = ieee_type(table[i].to_id);
            table[i].decl = verilog_func(ident_new(table[i].func));
         }
      });

   for (int i = 0; i < ARRAY_LEN(table); i++) {
      if (type_eq(table[i].from, from) && type_eq(table[i].to, to))
         return table[i].decl;
   }

   return NULL;
}

static tree_t elab_to_verilog(type_t from, type_t to)
{
   static struct {
      const ieee_type_t    from_id;
      const verilog_type_t to_id;
      const char *const    func;
      type_t               from;
      type_t               to;
      tree_t               decl;
   } table[] = {
      { IEEE_STD_ULOGIC, VERILOG_LOGIC, "NVC.VERILOG.TO_VERILOG(U)" T_LOGIC },
      { IEEE_STD_ULOGIC, VERILOG_NET_VALUE,
        "NVC.VERILOG.TO_VERILOG(U)" T_NET_VALUE }
   };

   INIT_ONCE({
         for (int i = 0; i < ARRAY_LEN(table); i++) {
           table[i].from = ieee_type(table[i].from_id);
           table[i].to   = verilog_type(table[i].to_id);
           table[i].decl = verilog_func(ident_new(table[i].func));
         }
      });

   for (int i = 0; i < ARRAY_LEN(table); i++) {
      if (type_eq(table[i].from, from) && type_eq(table[i].to, to))
         return table[i].decl;
   }

   return NULL;
}

static tree_t elab_mixed_binding(tree_t comp, mod_cache_t *mc)
{
   assert(tree_kind(comp) == T_COMPONENT);

   tree_t bind = tree_new(T_BINDING);
   tree_set_ident(bind, vlog_ident(mc->module));
   tree_set_loc(bind, tree_loc(comp));
   tree_set_ref(bind, mc->wrap);
   tree_set_class(bind, C_ENTITY);

   const int nports = tree_ports(comp);
   const int ndecls = vlog_decls(mc->module);

   bit_mask_t have;
   mask_init(&have, nports);

   bool have_named = false;
   for (int i = 0; i < ndecls; i++) {
      vlog_node_t mport = vlog_decl(mc->module, i);
      if (vlog_kind(mport) != V_PORT_DECL)
         continue;

      ident_t name = vlog_ident2(mport);

      tree_t vport = tree_port(mc->block, i);
      assert(tree_ident(vport) == vlog_ident(mport));

      tree_t cport = NULL;
      for (int j = 0; j < nports; j++) {
         tree_t pj = tree_port(comp, j);
         if (tree_ident(pj) == name) {
            cport = pj;
            mask_set(&have, j);
            break;
         }
      }

      if (cport == NULL) {
         error_at(tree_loc(comp), "missing matching VHDL port declaration for "
                  "Verilog port %s in component %s", istr(vlog_ident(mport)),
                  istr(tree_ident(comp)));
         return NULL;
      }

      if (name != tree_ident(cport)) {
         error_at(tree_loc(cport), "expected VHDL port name %s to match "
                  "Verilog port name %s in component %s",
                  istr(tree_ident(cport)), istr(vlog_ident(mport)),
                  istr(tree_ident(comp)));
         return NULL;
      }

      type_t btype = tree_type(cport);
      type_t vtype = tree_type(vport);

      if (vlog_subkind(mport) == V_PORT_INPUT) {
         tree_t func = elab_to_verilog(btype, vtype);
         if (func == NULL) {
            error_at(tree_loc(cport), "cannot connect VHDL signal with type "
                     "%s to Verilog input port %s", type_pp(btype),
                     istr(vlog_ident(mport)));
            return NULL;
         }

         tree_t conv = tree_new(T_CONV_FUNC);
         tree_set_loc(conv, tree_loc(cport));
         tree_set_ref(conv, func);
         tree_set_ident(conv, tree_ident(func));
         tree_set_type(conv, type_result(tree_type(func)));
         tree_set_value(conv, make_ref(cport));

         if (have_named)
            add_param(bind, conv, P_NAMED, make_ref(vport));
         else
            add_param(bind, conv, P_POS, NULL);
      }
      else {
         tree_t func = elab_to_vhdl(vtype, btype);
         if (func == NULL) {
            error_at(tree_loc(cport), "cannot connect VHDL signal with type "
                     "%s to Verilog output port %s", type_pp(btype),
                     istr(vlog_ident(mport)));
            return NULL;
         }

         tree_t conv = tree_new(T_CONV_FUNC);
         tree_set_loc(conv, tree_loc(cport));
         tree_set_ref(conv, func);
         tree_set_ident(conv, tree_ident(func));
         tree_set_type(conv, type_result(tree_type(func)));
         tree_set_value(conv, make_ref(vport));

         add_param(bind, make_ref(cport), P_NAMED, conv);
         have_named = true;
      }
   }

   for (int i = 0; i < nports; i++) {
      if (!mask_test(&have, i)) {
         tree_t p = tree_port(comp, i);
         diag_t *d = diag_new(DIAG_ERROR, tree_loc(p));
         diag_printf(d, "port %s not found in Verilog module %s",
                     istr(tree_ident(p)), istr(vlog_ident2(mc->module)));
         diag_emit(d);
      }
   }

   mask_free(&have);

   return bind;
}

static tree_t elab_verilog_conversion(type_t from, type_t to)
{
   static struct {
      const verilog_type_t from_id;
      const verilog_type_t to_id;
      const char *const    func;
      type_t               from;
      type_t               to;
      tree_t               decl;
   } table[] = {
      { VERILOG_NET_VALUE, VERILOG_LOGIC,
        "NVC.VERILOG.TO_LOGIC(" T_NET_VALUE ")" T_LOGIC },
      { VERILOG_NET_ARRAY, VERILOG_LOGIC_ARRAY,
        "NVC.VERILOG.TO_LOGIC(" T_NET_ARRAY ")" T_LOGIC_ARRAY },
      { VERILOG_WIRE_ARRAY, VERILOG_LOGIC_ARRAY,
        "NVC.VERILOG.TO_LOGIC(" T_WIRE_ARRAY ")" T_LOGIC_ARRAY },
      { VERILOG_LOGIC, VERILOG_NET_VALUE,
        "NVC.VERILOG.TO_NET(" T_LOGIC ")" T_NET_VALUE },
      { VERILOG_LOGIC_ARRAY, VERILOG_NET_ARRAY,
        "NVC.VERILOG.TO_NET(" T_LOGIC_ARRAY ")" T_NET_ARRAY },
      { VERILOG_LOGIC_ARRAY, VERILOG_WIRE_ARRAY,
        "NVC.VERILOG.TO_NET(" T_LOGIC_ARRAY ")" T_WIRE_ARRAY },
   };

   INIT_ONCE({
         for (int i = 0; i < ARRAY_LEN(table); i++) {
            table[i].from = verilog_type(table[i].from_id);
            table[i].to   = verilog_type(table[i].to_id);
            table[i].decl = verilog_func(ident_new(table[i].func));
         }
      });

   for (int i = 0; i < ARRAY_LEN(table); i++) {
      if (type_eq(table[i].from, from) && type_eq(table[i].to, to))
         return table[i].decl;
   }

   return NULL;
}

static tree_t elab_verilog_binding(vlog_node_t inst, mod_cache_t *mc,
                                   const elab_ctx_t *ctx)
{
   assert(vlog_kind(inst) == V_MOD_INST);

   tree_t bind = tree_new(T_BINDING);
   tree_set_ident(bind, vlog_ident(mc->module));
   tree_set_loc(bind, vlog_loc(inst));
   tree_set_ref(bind, mc->wrap);
   tree_set_class(bind, C_ENTITY);

   const int nports = vlog_ports(mc->module);
   const int nparams = vlog_params(inst);
   const int outports = tree_ports(ctx->out);
   const int outdecls = tree_decls(ctx->out);

   if (nports != nparams) {
      error_at(vlog_loc(inst), "expected %d port connections for module %s "
               "but found %d", nports, istr(vlog_ident(mc->module)), nparams);
      return NULL;
   }

   bool have_named = false;
   for (int i = 0; i < nports; i++) {
      vlog_node_t conn = vlog_param(inst, i);
      assert(vlog_kind(conn) == V_REF);

      ident_t id = vlog_ident(conn);
      tree_t decl = NULL;

      for (int j = 0; j < outports; j++) {
         tree_t p = tree_port(ctx->out, j);
         if (tree_ident(p) == id) {
            decl = p;
            break;
         }
      }

      if (decl == NULL) {
         for (int j = 0; j < outdecls; j++) {
            tree_t d = tree_decl(ctx->out, j);
            if (tree_ident(d) == id) {
               decl = d;
               break;
            }
         }
      }

      assert(decl != NULL);

      tree_t port = tree_port(mc->block, i);

      type_t dtype = tree_type(decl);
      type_t ptype = tree_type(port);

      if (type_eq(dtype, ptype)) {
         if (have_named)
            add_param(bind, make_ref(decl), P_NAMED, make_ref(port));
         else
            add_param(bind, make_ref(decl), P_POS, NULL);
      }
      else if (tree_subkind(port) == PORT_IN) {
         tree_t func = elab_verilog_conversion(dtype, ptype);
         assert(func != NULL);

         tree_t conv = tree_new(T_CONV_FUNC);
         tree_set_loc(conv, vlog_loc(conn));
         tree_set_ref(conv, func);
         tree_set_ident(conv, tree_ident(func));
         tree_set_type(conv, type_result(tree_type(func)));
         tree_set_value(conv, make_ref(decl));

         if (have_named)
            add_param(bind, conv, P_NAMED, make_ref(port));
         else
            add_param(bind, conv, P_POS, NULL);
      }
      else {
         tree_t func = elab_verilog_conversion(ptype, dtype);
         assert(func != NULL);

         tree_t conv = tree_new(T_CONV_FUNC);
         tree_set_loc(conv, vlog_loc(conn));
         tree_set_ref(conv, func);
         tree_set_ident(conv, tree_ident(func));
         tree_set_type(conv, type_result(tree_type(func)));
         tree_set_value(conv, make_ref(port));

         add_param(bind, make_ref(decl), P_NAMED, conv);
         have_named = true;
      }
   }

   return bind;
}

static tree_t elab_default_binding(tree_t inst, const elab_ctx_t *ctx)
{
   // Default binding indication is described in LRM 93 section 5.2.2

   tree_t comp = tree_ref(inst);

   ident_t full_i = tree_ident(comp);
   ident_t lib_i = ident_until(full_i, '.');

   lib_t lib = NULL;
   bool synth_binding = true;
   if (lib_i == full_i) {
      lib    = ctx->library;
      full_i = ident_prefix(lib_name(lib), full_i, '.');
   }
   else {
      synth_binding = false;
      lib = elab_find_lib(lib_i, ctx);

      // Strip out the component package name, if any
      full_i = ident_prefix(lib_i, ident_rfrom(full_i, '.'), '.');
   }

   object_t *obj = lib_get_generic(lib, full_i, NULL);

   vlog_node_t mod = vlog_from_object(obj);
   if (mod != NULL) {
      mod_cache_t *mc = elab_cached_module(mod, ctx);
      return elab_mixed_binding(comp, mc);
   }

   tree_t entity = tree_from_object(obj);

   if (entity == NULL && synth_binding) {
      // This is not correct according to the LRM but matches the
      // behaviour of many synthesis tools
      synth_binding_params_t params = {
         .comp     = comp,
         .result   = &entity
      };
      lib_for_all(elab_synth_binding_cb, &params);
   }

   if (entity == NULL) {
      warn_at(tree_loc(inst), "cannot find entity for component %s "
              "without binding indication", istr(tree_ident(comp)));
      return NULL;
   }

   tree_t arch = elab_pick_arch(tree_loc(comp), entity, ctx);

   // Check entity is compatible with component declaration

   tree_t bind = tree_new(T_BINDING);
   tree_set_ident(bind, tree_ident(arch));
   tree_set_loc(bind, tree_loc(arch));
   tree_set_ref(bind, arch);
   tree_set_class(bind, C_ENTITY);

   const int c_ngenerics = tree_generics(comp);
   const int e_ngenerics = tree_generics(entity);

   for (int i = 0; i < e_ngenerics; i++) {
      tree_t eg = tree_generic(entity, i);

      tree_t match = NULL;
      for (int j = 0; j < c_ngenerics; j++) {
         tree_t cg = tree_generic(comp, j);
         if (ident_casecmp(tree_ident(eg), tree_ident(cg))) {
            match = cg;
            break;
         }
      }

      tree_t value;
      if (match != NULL) {
         const class_t class = tree_class(eg);

         if (class != tree_class(match)) {
            diag_t *d = diag_new(DIAG_ERROR, tree_loc(inst));
            diag_printf(d, "generic %s in component %s has class %s which is "
                        "incompatible with class %s in entity %s",
                        istr(tree_ident(match)), istr(tree_ident(comp)),
                        class_str(tree_class(match)), class_str(class),
                        istr(tree_ident(entity)));
            diag_hint(d, tree_loc(match), "declaration of generic %s in "
                      "component", istr(tree_ident(match)));
            diag_hint(d, tree_loc(eg), "declaration of generic %s in entity",
                      istr(tree_ident(eg)));
            diag_emit(d);
            return NULL;
         }
         else if (class == C_PACKAGE) {
            value = tree_new(T_REF);
            tree_set_ident(value, tree_ident(match));
            tree_set_ref(value, match);
         }
         else {
            type_t ctype = tree_type(match);
            type_t etype = tree_type(eg);
            if (!type_eq(ctype, etype)) {
               diag_t *d = diag_new(DIAG_ERROR, tree_loc(inst));
               diag_printf(d, "generic %s in component %s has type %s which is "
                           "incompatible with type %s in entity %s",
                           istr(tree_ident(match)), istr(tree_ident(comp)),
                           type_pp2(ctype, etype), type_pp2(etype, ctype),
                           istr(tree_ident(entity)));
               diag_hint(d, tree_loc(match), "declaration of generic %s in "
                         "component", istr(tree_ident(match)));
               diag_hint(d, tree_loc(eg), "declaration of generic %s in entity",
                         istr(tree_ident(eg)));
               diag_emit(d);
               return NULL;
            }

            value = make_ref(match);
         }
      }
      else if (tree_has_value(eg)) {
         tree_t def = tree_value(eg);
         if (is_literal(def))
            value = def;
         else {
            tree_t open = tree_new(T_OPEN);
            tree_set_loc(open, tree_loc(eg));
            tree_set_type(open, tree_type(eg));

            value = open;
         }
      }
      else {
         diag_t *d = diag_new(DIAG_ERROR, tree_loc(inst));
         diag_printf(d, "generic %s in entity %s without a default value "
                     "has no corresponding generic in component %s",
                     istr(tree_ident(eg)),  istr(tree_ident(entity)),
                     istr(tree_ident(comp)));
         diag_hint(d, tree_loc(eg), "declaration of generic %s in entity",
                   istr(tree_ident(eg)));
         diag_emit(d);
         return NULL;
      }

      tree_t map = tree_new(T_PARAM);
      tree_set_loc(map, tree_loc(inst));
      tree_set_value(map, value);
      tree_set_subkind(map, P_POS);
      tree_set_pos(map, i);

      tree_add_genmap(bind, map);
   }

   const int c_nports = tree_ports(comp);
   const int e_nports = tree_ports(entity);

   for (int i = 0; i < e_nports; i++) {
      tree_t ep = tree_port(entity, i);

      tree_t match = NULL;
      for (int j = 0; j < c_nports; j++) {
         tree_t cp = tree_port(comp, j);
         if (ident_casecmp(tree_ident(ep), tree_ident(cp))) {
            match = cp;
            break;
         }
      }

      tree_t value;
      if (match != NULL) {
         type_t ctype = tree_type(match);
         type_t etype = tree_type(ep);
         if (!type_eq(ctype, etype)) {
            diag_t *d = diag_new(DIAG_ERROR, tree_loc(inst));
            diag_printf(d, "port %s in component %s has type %s which is "
                        "incompatible with type %s in entity %s",
                        istr(tree_ident(match)), istr(tree_ident(comp)),
                        type_pp2(ctype, etype), type_pp2(etype, ctype),
                        istr(tree_ident(entity)));
            diag_hint(d, tree_loc(match), "declaration of port %s in component",
                      istr(tree_ident(match)));
            diag_hint(d, tree_loc(ep), "declaration of port %s in entity",
                      istr(tree_ident(ep)));
            diag_emit(d);
            return NULL;
         }

         value = make_ref(match);
      }
      else {
         const bool open_ok =
            tree_has_value(ep)
            || (tree_subkind(ep) == PORT_OUT
                && !type_is_unconstrained(tree_type(ep)));

          if (open_ok) {
             tree_t open = tree_new(T_OPEN);
             tree_set_loc(open, tree_loc(ep));
             tree_set_type(open, tree_type(ep));

             value = open;
          }
          else {
            diag_t *d = diag_new(DIAG_ERROR, tree_loc(inst));
            diag_printf(d, "port %s in entity %s without a default value "
                        "has no corresponding port in component %s",
                        istr(tree_ident(ep)), istr(tree_ident(entity)),
                        istr(tree_ident(comp)));
            diag_hint(d, tree_loc(ep), "port %s declared here",
                      istr(tree_ident(ep)));
            diag_emit(d);
            return NULL;
         }
      }

      add_param(bind, value, P_POS, NULL);
   }

   return bind;
}

static void elab_write_generic(text_buf_t *tb, tree_t value)
{
   switch (tree_kind(value)) {
   case T_LITERAL:
      switch (tree_subkind(value)) {
      case L_INT:  tb_printf(tb, "%"PRIi64, tree_ival(value)); break;
      case L_REAL: tb_printf(tb, "%lf", tree_dval(value)); break;
      case L_PHYSICAL:
         tb_printf(tb, "%"PRIi64" %s", tree_ival(value),
                   istr(tree_ident(value)));
         break;
      }
      break;
   case T_STRING:
      {
         tb_printf(tb, "\"");
         const int nchars = tree_chars(value);
         for (int i = 0; i < nchars; i++)
            tb_append(tb, ident_char(tree_ident(tree_char(value, i)), 1));
         tb_printf(tb, "\"");
      }
      break;
   case T_AGGREGATE:
      {
         tb_append(tb, '(');
         const int nassocs = tree_assocs(value);
         for (int i = 0; i < nassocs; i++) {
            if (i > 0) tb_cat(tb, ", ");
            elab_write_generic(tb, tree_value(tree_assoc(value, i)));
         }
         tb_append(tb, ')');
      }
      break;
   case T_REF:
      if (is_subprogram(tree_ref(value)))
         tb_printf(tb, "%s", type_pp(tree_type(value)));
      else
         tb_printf(tb, "%s", istr(tree_ident(value)));
      break;
   case T_TYPE_CONV:
   case T_QUALIFIED:
      elab_write_generic(tb, tree_value(value));
      break;
   case T_TYPE_REF:
      tb_printf(tb, "%s", type_pp(tree_type(value)));
      break;
   case T_OPEN:
      tb_cat(tb, "OPEN");
      break;
   default:
      tb_printf(tb, "...");
      DEBUG_ONLY(tb_cat(tb, tree_kind_str(tree_kind(value))));
   }
}

static void elab_hint_fn(diag_t *d, void *arg)
{
   tree_t t = arg;

   diag_hint(d, tree_loc(t), "while elaborating instance %s",
             istr(tree_ident(t)));

   tree_t unit = tree_ref(t);
   const tree_kind_t kind = tree_kind(unit);
   if (kind == T_CONFIGURATION || kind == T_ARCH)
      unit = tree_primary(unit);

   const int ngenerics = tree_genmaps(t);
   for (int i = 0; i < ngenerics; i++) {
      tree_t p = tree_genmap(t, i);
      ident_t name = NULL;
      switch (tree_subkind(p)) {
      case P_POS:
         name = tree_ident(tree_generic(unit, tree_pos(p)));
         break;
      case P_NAMED:
         name = tree_ident(tree_name(p));
         break;
      default:
         continue;
      }

      LOCAL_TEXT_BUF tb = tb_new();
      elab_write_generic(tb, tree_value(p));
      diag_hint(d, NULL, "generic %s => %s", istr(name), tb_get(tb));
   }
}

static void elab_ports(tree_t entity, tree_t bind, const elab_ctx_t *ctx)
{
   const int nports = tree_ports(entity);
   const int nparams = tree_params(bind);
   bool have_named = false;

   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(entity, i), map = NULL;
      ident_t pname = tree_ident(p);

      if (i < nparams && !have_named) {
         tree_t m = tree_param(bind, i);
         if (tree_subkind(m) == P_POS) {
            assert(tree_pos(m) == i);
            tree_add_param(ctx->out, m);
            map = m;
         }
      }

      if (map == NULL) {
         for (int j = 0; j < nparams; j++) {
            tree_t m = tree_param(bind, j);
            if (tree_subkind(m) == P_NAMED) {
               tree_t name = tree_name(m), ref;
               bool is_conv = false;

               switch (tree_kind(name)) {
               case T_TYPE_CONV:
               case T_CONV_FUNC:
                  is_conv = true;
                  ref = name_to_ref(tree_value(name));
                  break;
               default:
                  ref = name_to_ref(name);
                  break;
               }
               assert(ref != NULL);

               if (tree_ident(ref) != pname)
                  continue;

               map = tree_new(T_PARAM);
               tree_set_loc(map, tree_loc(m));
               tree_set_value(map, tree_value(m));

               tree_add_param(ctx->out, map);

               if (!have_named && !is_conv && ref == name) {
                  tree_set_subkind(map, P_POS);
                  tree_set_pos(map, i);
                  break;
               }
               else {
                  tree_set_subkind(map, P_NAMED);
                  tree_set_name(map, change_ref(tree_name(m), p));
                  have_named = true;
               }
            }
         }
      }

      if (map == NULL) {
         map = tree_new(T_PARAM);
         tree_set_loc(map, tree_loc(p));

         if (have_named) {
            tree_set_subkind(map, P_NAMED);
            tree_set_name(map, make_ref(p));
         }
         else {
            tree_set_subkind(map, P_POS);
            tree_set_pos(map, i);
         }

         tree_t open = tree_new(T_OPEN);
         tree_set_type(open, tree_type(p));
         tree_set_loc(open, tree_loc(p));

         tree_set_value(map, open);

         tree_add_param(ctx->out, map);
      }

      tree_add_port(ctx->out, p);
   }
}

static tree_t elab_parse_generic_string(tree_t generic, const char *str)
{
   type_t type = tree_type(generic);

   parsed_value_t value;
   if (!parse_value(type, str, &value))
      fatal("failed to parse \"%s\" as type %s for generic %s",
            str, type_pp(type), istr(tree_ident(generic)));

   if (type_is_enum(type)) {
      type_t base = type_base_recur(type);
      tree_t lit = type_enum_literal(base, value.integer);

      tree_t result = tree_new(T_REF);
      tree_set_type(result, type);
      tree_set_ident(result, ident_new(str));
      tree_set_ref(result, lit);
      tree_set_loc(result, tree_loc(generic));

      return result;
   }
   else if (type_is_integer(type)) {
      tree_t result = tree_new(T_LITERAL);
      tree_set_subkind(result, L_INT);
      tree_set_type(result, type);
      tree_set_ival(result, value.integer);
      tree_set_loc(result, tree_loc(generic));

      return result;
   }
   else if (type_is_real(type)) {
      tree_t result = tree_new(T_LITERAL);
      tree_set_subkind(result, L_REAL);
      tree_set_type(result, type);
      tree_set_dval(result, value.real);
      tree_set_loc(result, tree_loc(generic));

      return result;
   }
   else if (type_is_physical(type)) {
      tree_t result = tree_new(T_LITERAL);
      tree_set_subkind(result, L_PHYSICAL);
      tree_set_type(result, type);
      tree_set_ival(result, value.integer);
      tree_set_loc(result, tree_loc(generic));

      return result;
   }
   else if (type_is_character_array(type)) {
      tree_t t = tree_new(T_STRING);
      tree_set_loc(t, tree_loc(generic));

      type_t elem = type_base_recur(type_elem(type));
      for (int i = 0; i < value.enums->count; i++) {
         tree_t lit = type_enum_literal(elem, value.enums->values[i]);

         tree_t ref = tree_new(T_REF);
         tree_set_ident(ref, tree_ident(lit));
         tree_set_ref(ref, lit);
         tree_add_char(t, ref);
      }
      free(value.enums);

      tree_set_type(t, subtype_for_string(t, type));
      return t;
   }
   else
      fatal("cannot override generic %s of type %s", istr(tree_ident(generic)),
            type_pp(type));
}

static tree_t elab_find_generic_override(tree_t g, const elab_ctx_t *ctx)
{
   if (generic_override == NULL)
      return NULL;

   ident_t qual = tree_ident(g);
   for (const elab_ctx_t *e = ctx; e->inst; e = e->parent)
      qual = ident_prefix(tree_ident(e->inst), qual, '.');

   generic_list_t **it, *tmp;
   for (it = &generic_override;
        *it && (*it)->name != qual;
        it = &((*it)->next))
      ;

   if (*it == NULL)
      return NULL;

   tree_t value = elab_parse_generic_string(g, (*it)->value);

   *it = (tmp = (*it))->next;
   free(tmp->value);
   free(tmp);

   return value;
}

static void elab_generics(tree_t entity, tree_t bind, elab_ctx_t *ctx)
{
   const int ngenerics = tree_generics(entity);
   const int ngenmaps = tree_genmaps(bind);

   for (int i = 0; i < ngenerics; i++) {
      tree_t g = tree_generic(entity, i);
      tree_add_generic(ctx->out, g);

      tree_t map = NULL;
      if (i < ngenmaps) {
         map = tree_genmap(bind, i);
         assert(tree_subkind(map) == P_POS);
         assert(tree_pos(map) == i);
      }
      else if (tree_has_value(g)) {
         map = tree_new(T_PARAM);
         tree_set_loc(map, tree_loc(g));
         tree_set_subkind(map, P_POS);
         tree_set_pos(map, i);
         tree_set_value(map, tree_value(g));
      }

      tree_t override = elab_find_generic_override(g, ctx);
      if (override != NULL) {
         map = tree_new(T_PARAM);
         tree_set_subkind(map, P_POS);
         tree_set_pos(map, i);
         tree_set_value(map, override);
      }

      if (map == NULL) {
         error_at(tree_loc(bind), "missing value for generic %s with no "
                  "default", istr(tree_ident(g)));
         continue;
      }

      tree_t value = tree_value(map);

      switch (tree_kind(value)) {
      case T_REF:
         if (tree_kind(tree_ref(value)) == T_ENUM_LIT)
            break;
         else if (tree_class(g) == C_PACKAGE)
            break;
         // Fall-through
      case T_ARRAY_REF:
      case T_RECORD_REF:
      case T_FCALL:
         if (type_is_scalar(tree_type(value))) {
            void *context = NULL;
            if (ctx->parent->scope->kind != SCOPE_ROOT)
               context = *mptr_get(ctx->parent->scope->privdata);

            tree_t folded = eval_try_fold(ctx->jit, value,
                                          ctx->registry,
                                          ctx->parent->lowered,
                                          context);

            if (folded != value) {
               tree_t m = tree_new(T_PARAM);
               tree_set_loc(m, tree_loc(map));
               tree_set_subkind(m, P_POS);
               tree_set_pos(m, tree_pos(map));
               tree_set_value(m, (value = folded));

               map = m;
            }
         }
         break;

      default:
         break;
      }

      tree_add_genmap(ctx->out, map);

      if (is_literal(value)) {
         // These values can be safely substituted for all references to
         // the generic name
         if (ctx->generics == NULL)
            ctx->generics = hash_new(ngenerics * 2);
         hash_put(ctx->generics, g, value);
      }
   }
}

static void elab_map_generic_type(type_t generic, type_t actual, hash_t *map)
{
   assert(type_kind(generic) == T_GENERIC);

   switch (type_subkind(generic)) {
   case GTYPE_ARRAY:
      {
         type_t gelem = type_elem(generic);
         if (type_kind(gelem) == T_GENERIC && !type_has_ident(gelem))
            elab_map_generic_type(gelem, type_elem(actual), map);

         const int ndims = type_indexes(generic);
         for (int i = 0; i < ndims; i++) {
            type_t index = type_index(generic, i);
            if (type_kind(index) == T_GENERIC && !type_has_ident(index))
               elab_map_generic_type(index, index_type_of(actual, i), map);
         }
      }
      break;
   }

   hash_put(map, generic, actual);
}

static void elab_instance_fixup(tree_t arch, const elab_ctx_t *ctx)
{
   if (standard() < STD_08)
      return;

   hash_t *map = NULL;

   const int ngenerics = tree_generics(ctx->out);
   assert(tree_genmaps(ctx->out) == ngenerics);

   for (int i = 0; i < ngenerics; i++) {
      tree_t g = tree_generic(ctx->out, i);

      const class_t class = tree_class(g);
      if (class == C_CONSTANT)
         continue;
      else if (map == NULL)
         map = hash_new(64);

      tree_t value = tree_value(tree_genmap(ctx->out, i));

      switch (class) {
      case C_TYPE:
         elab_map_generic_type(tree_type(g), tree_type(value), map);
         break;

      case C_PACKAGE:
         {
            tree_t formal = tree_ref(tree_value(g));
            tree_t actual = tree_ref(value);

            const int ndecls = tree_decls(formal);
            for (int i = 0; i < ndecls; i++) {
               tree_t gd = tree_decl(formal, i);
               tree_t ad = tree_decl(actual, i);
               assert(tree_kind(gd) == tree_kind(ad));

               hash_put(map, gd, ad);

               if (is_type_decl(gd))
                  hash_put(map, tree_type(gd), tree_type(ad));
            }

            const int ngenerics = tree_generics(formal);
            for (int i = 0; i < ngenerics; i++) {
               tree_t fg = tree_generic(formal, i);
               tree_t ag = tree_generic(actual, i);

               switch (tree_class(fg)) {
               case C_FUNCTION:
               case C_PROCEDURE:
                  {
                     // Get the actual subprogram from the generic map
                     assert(ngenerics == tree_genmaps(actual));
                     tree_t ref = tree_value(tree_genmap(actual, i));
                     assert(tree_kind(ref) == T_REF);

                     hash_put(map, fg, tree_ref(ref));
                  }
                  break;
               case C_TYPE:
                  hash_put(map, tree_type(fg), tree_type(ag));
                  break;
               case C_PACKAGE:
                  // TODO: this should be processed recursively
               default:
                  hash_put(map, fg, ag);
                  break;
               }
            }

            hash_put(map, g, actual);
         }
         break;

      case C_FUNCTION:
      case C_PROCEDURE:
         hash_put(map, g, tree_ref(value));
         break;

      default:
         break;
      }
   }

   if (map == NULL)
      return;

   instance_fixup(arch, map);
   hash_free(map);
}

static void elab_context(tree_t t)
{
   const int nctx = tree_contexts(t);
   for (int i = 0; i < nctx; i++) {
      // Make sure any referenced libraries are loaded to allow synth
      // binding to search for entities in them
      tree_t c = tree_context(t, i);
      if (tree_kind(c) == T_LIBRARY)
         lib_require(tree_ident(c));
   }
}

static void elab_inherit_context(elab_ctx_t *ctx, const elab_ctx_t *parent)
{
   ctx->parent    = parent;
   ctx->jit       = parent->jit;
   ctx->registry  = parent->registry;
   ctx->mir       = parent->mir;
   ctx->root      = parent->root;
   ctx->dotted    = ctx->dotted ?: parent->dotted;
   ctx->inst_name = ctx->inst_name ?: parent->inst_name;
   ctx->library   = ctx->library ?: parent->library;
   ctx->out       = ctx->out ?: parent->out;
   ctx->cover     = parent->cover;
   ctx->sdf       = parent->sdf;
   ctx->inst      = ctx->inst ?: parent->inst;
   ctx->modcache  = parent->modcache;
   ctx->depth     = parent->depth + 1;
   ctx->model     = parent->model;
}

static driver_set_t *elab_driver_set(const elab_ctx_t *ctx)
{
   if (ctx->drivers != NULL)
      return ctx->drivers;
   else if (ctx->parent != NULL)
      return elab_driver_set(ctx->parent);
   else
      return NULL;
}

static void elab_lower(tree_t b, vcode_unit_t shape, elab_ctx_t *ctx)
{
   ctx->lowered = lower_instance(ctx->registry, ctx->parent->lowered, shape,
                                 elab_driver_set(ctx), ctx->cover, b);

   if (ctx->inst != NULL)
      diag_add_hint_fn(elab_hint_fn, ctx->inst);

   ctx->scope = create_scope(ctx->model, b, ctx->parent->scope);

   if (ctx->inst != NULL)
      diag_remove_hint_fn(elab_hint_fn);
}

static void elab_verilog_module(tree_t bind, ident_t label, mod_cache_t *mc,
                                const elab_ctx_t *ctx)
{
   const char *label_str = istr(label);
   ident_t ninst = hpathf(ctx->inst_name, ':', "%s", label_str);
   ident_t ndotted = ident_prefix(ctx->dotted, label, '.');

   elab_ctx_t new_ctx = {
      .inst_name = ninst,
      .dotted    = ndotted,
   };
   elab_inherit_context(&new_ctx, ctx);

   tree_t b = tree_new(T_BLOCK);
   tree_set_ident(b, label);
   tree_set_loc(b, tree_loc(ctx->out));

   tree_add_stmt(ctx->out, b);
   new_ctx.out = b;

   elab_push_scope(mc->wrap, &new_ctx);

   if (bind != NULL)
      elab_ports(mc->block, bind, &new_ctx);

   if (error_count() == 0)
      elab_decls(mc->block, &new_ctx);

   if (error_count() == 0) {
      new_ctx.drivers = find_drivers(mc->block);
      elab_lower(b, mc->shape, &new_ctx);
      elab_stmts(mc->block, &new_ctx);
   }

   elab_pop_scope(&new_ctx);
}

static void elab_architecture(tree_t bind, tree_t arch, tree_t config,
                              const elab_ctx_t *ctx)
{
   tree_t inst = NULL;
   ident_t label, ninst = NULL;
   switch (tree_kind(bind)) {
   case T_BINDING:
      label = ident_rfrom(tree_ident(tree_primary(arch)), '.');
      break;
   case T_INSTANCE:
      {
         label = tree_ident(bind);
         inst = bind;
         ninst = hpathf(ctx->inst_name, ':', "%s@%s(%s)", istr(label),
                        simple_name(istr(tree_ident2(arch))),
                        simple_name(istr(tree_ident(arch))));
      }
      break;
   default:
      fatal_trace("unexpected binding kind %s in elab_architecture",
                  tree_kind_str(tree_kind(bind)));
   }

   ident_t ndotted = ident_prefix(ctx->dotted, label, '.');

   elab_ctx_t new_ctx = {
      .inst_name = ninst,
      .dotted    = ndotted,
      .inst      = inst,
   };
   elab_inherit_context(&new_ctx, ctx);

   tree_t b = tree_new(T_BLOCK);
   tree_set_ident(b, label);
   tree_set_loc(b, tree_loc(bind));

   tree_add_stmt(ctx->out, b);
   new_ctx.out = b;

   new_ctx.library = lib_require(ident_until(tree_ident(arch), '.'));

   elab_subprogram_prefix(arch, &new_ctx);

   tree_t arch_copy;
   if (config != NULL) {
      assert(tree_ref(config) == arch);
      new_ctx.config = elab_copy(config, &new_ctx);
      arch_copy = tree_ref(new_ctx.config);
   }
   else
      arch_copy = elab_copy(arch, &new_ctx);

   tree_t entity = tree_primary(arch_copy);

   elab_push_scope(arch, &new_ctx);
   elab_context(entity);
   elab_context(arch_copy);
   elab_generics(entity, bind, &new_ctx);
   elab_instance_fixup(arch_copy, &new_ctx);
   simplify_global(arch_copy, new_ctx.generics, ctx->jit, ctx->registry,
                   ctx->mir);
   elab_ports(entity, bind, &new_ctx);
   elab_decls(entity, &new_ctx);

   if (error_count() == 0)
      elab_decls(arch_copy, &new_ctx);

   if (error_count() == 0) {
      new_ctx.drivers = find_drivers(arch_copy);
      elab_lower(b, NULL, &new_ctx);
      elab_stmts(entity, &new_ctx);
      elab_stmts(arch_copy, &new_ctx);
   }

   elab_pop_scope(&new_ctx);
}

static tree_t elab_find_spec(tree_t inst, const elab_ctx_t *ctx)
{
   if (tree_has_spec(inst))
      return tree_spec(inst);
   else if (ctx->config == NULL)
      return NULL;

   assert(tree_kind(ctx->config) == T_BLOCK_CONFIG);

   tree_t spec = NULL;
   const int ndecls = tree_decls(ctx->config);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(ctx->config, i);
      if (tree_kind(d) != T_SPEC)
         continue;
      else if (tree_ident2(d) != tree_ident2(inst))
         continue;

      bool apply = false;
      if (tree_has_ident(d)) {
         ident_t match = tree_ident(d);
         apply = (match == tree_ident(inst) || match == well_known(W_ALL));
      }
      else if (spec == NULL)
         apply = true;

      if (apply) spec = d;
   }

   return spec;
}

static void elab_component(tree_t inst, tree_t comp, const elab_ctx_t *ctx)
{
   tree_t arch = NULL, config = NULL, bind = NULL, spec;
   if ((spec = elab_find_spec(inst, ctx)) && tree_has_value(spec)) {
      bind = tree_value(spec);
      assert(tree_kind(bind) == T_BINDING);

      const int ndecls = tree_decls(spec);
      if (ndecls == 0) {
         tree_t unit = tree_ref(bind);
         switch (tree_kind(unit)) {
         case T_ENTITY:
            arch = elab_pick_arch(tree_loc(inst), unit, ctx);
            break;
         case T_CONFIGURATION:
            config = tree_decl(unit, 0);
            assert(tree_kind(config) == T_BLOCK_CONFIG);
            arch = tree_ref(config);
            break;
         case T_ARCH:
            arch = unit;
            break;
         default:
            fatal_at(tree_loc(bind), "sorry, this form of binding indication "
                     "is not supported yet");
         }
      }
      else {
         assert(ndecls == 1);

         config = tree_decl(spec, 0);
         assert(tree_kind(config) == T_BLOCK_CONFIG);

         arch = tree_ref(config);
      }
   }
   else if (spec == NULL && (bind = elab_default_binding(inst, ctx)))
      arch = tree_ref(bind);

   // Must create a unique instance if type or package generics present
   const int ngenerics = tree_generics(comp);
   for (int i = 0; i < ngenerics; i++) {
      if (tree_class(tree_generic(comp, i)) != C_CONSTANT) {
         tree_t roots[] = { comp, bind };
         new_instance(roots, bind ? 2 : 1, ctx->dotted, ctx->prefix,
                      ARRAY_LEN(ctx->prefix));

         comp = roots[0];
         bind = roots[1];

         break;
      }
   }

   ident_t ninst = hpathf(ctx->inst_name, ':', "%s", istr(tree_ident(inst)));

   if (arch != NULL && tree_kind(arch) != T_VERILOG)
      ninst = hpathf(ninst, '@', "%s(%s)",
                     simple_name(istr(tree_ident2(arch))),
                     simple_name(istr(tree_ident(arch))));

   ident_t ndotted = ident_prefix(ctx->dotted, tree_ident(inst), '.');

   elab_ctx_t new_ctx = {
      .inst_name = ninst,
      .dotted    = ndotted,
      .inst      = inst,
   };
   elab_inherit_context(&new_ctx, ctx);

   tree_t b = tree_new(T_BLOCK);
   tree_set_ident(b, tree_ident(inst));
   tree_set_loc(b, tree_loc(inst));

   tree_add_stmt(ctx->out, b);
   new_ctx.out = b;

   elab_push_scope(comp, &new_ctx);
   elab_generics(comp, inst, &new_ctx);
   if (bind != NULL) elab_instance_fixup(bind, &new_ctx);
   elab_instance_fixup(comp, &new_ctx);
   elab_ports(comp, inst, &new_ctx);

   if (bind != NULL && tree_kind(arch) != T_VERILOG)
      new_ctx.drivers = find_drivers(bind);

   if (error_count() == 0)
      elab_lower(b, NULL, &new_ctx);

   if (arch == NULL)
      ;   // Unbound architecture
   else if (tree_kind(arch) == T_VERILOG) {
      mod_cache_t *mc = elab_cached_module(tree_vlog(arch), ctx);
      elab_verilog_module(bind, vlog_ident2(mc->module), mc, &new_ctx);
   }
   else if (error_count() == 0)
      elab_architecture(bind, arch, config, &new_ctx);

   elab_pop_scope(&new_ctx);
}

static tree_t elab_block_config(tree_t block, const elab_ctx_t *ctx)
{
   if (ctx->config == NULL)
      return NULL;

   ident_t label = tree_ident(block);

   const int ndecls = tree_decls(ctx->config);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(ctx->config, i);
      if (tree_kind(d) != T_BLOCK_CONFIG)
         continue;
      else if (tree_ident(d) == label)
         return d;
   }

   return NULL;
}

static void elab_instance(tree_t t, const elab_ctx_t *ctx)
{
   if (ctx->depth == MAX_DEPTH) {
      diag_t *d = diag_new(DIAG_ERROR, tree_loc(t));
      diag_printf(d, "maximum instantiation depth of %d reached", MAX_DEPTH);
      diag_hint(d, NULL, "this is likely caused by unbounded recursion");
      diag_emit(d);
      return;
   }

   tree_t ref = tree_ref(t);
   switch (tree_kind(ref)) {
   case T_ENTITY:
      {
         tree_t arch = elab_pick_arch(tree_loc(t), ref, ctx);
         elab_architecture(t, arch, NULL, ctx);
      }
      break;

   case T_ARCH:
      elab_architecture(t, ref, NULL, ctx);
      break;

   case T_COMPONENT:
      elab_component(t, ref, ctx);
      break;

   case T_CONFIGURATION:
      {
         tree_t config = tree_decl(ref, 0);
         assert(tree_kind(config) == T_BLOCK_CONFIG);

         tree_t arch = tree_ref(config);
         elab_architecture(t, arch, config, ctx);
      }
      break;

   default:
      fatal_trace("unexpected tree kind %s in elab_instance",
                  tree_kind_str(tree_kind(ref)));
   }
}

static void elab_decls(tree_t t, const elab_ctx_t *ctx)
{
   const int ndecls = tree_decls(t);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(t, i);

      switch (tree_kind(d)) {
      case T_SIGNAL_DECL:
      case T_IMPLICIT_SIGNAL:
      case T_ALIAS:
      case T_FILE_DECL:
      case T_VAR_DECL:
      case T_CONST_DECL:
      case T_FUNC_BODY:
      case T_PROC_BODY:
      case T_FUNC_INST:
      case T_PROC_INST:
      case T_PROT_DECL:
      case T_PROT_BODY:
      case T_TYPE_DECL:
      case T_SUBTYPE_DECL:
      case T_PACK_BODY:
      case T_PACKAGE:
      case T_PACK_INST:
      case T_PSL_DECL:
      case T_ATTR_SPEC:
         tree_add_decl(ctx->out, d);
         break;
      case T_FUNC_DECL:
      case T_PROC_DECL:
         if (!is_open_coded_builtin(tree_subkind(d)))
            tree_add_decl(ctx->out, d);
         break;
      default:
         break;
      }
   }
}

static void elab_push_scope(tree_t t, elab_ctx_t *ctx)
{
   tree_t h = tree_new(T_HIER);
   tree_set_loc(h, tree_loc(t));
   tree_set_subkind(h, tree_kind(t));
   tree_set_ref(h, t);

   tree_set_ident(h, ctx->inst_name);
   tree_set_ident2(h, ctx->dotted);

   tree_add_decl(ctx->out, h);
}

static void elab_pop_scope(elab_ctx_t *ctx)
{
   if (ctx->generics != NULL)
      hash_free(ctx->generics);

   if (ctx->drivers != NULL)
      free_drivers(ctx->drivers);

   if (ctx->lowered != NULL)
      unit_registry_finalise(ctx->registry, ctx->lowered);
}

static inline tree_t elab_eval_expr(tree_t t, const elab_ctx_t *ctx)
{
   void *context = *mptr_get(ctx->scope->privdata);
   return eval_must_fold(ctx->jit, t, ctx->registry, ctx->lowered, context);
}

static bool elab_copy_genvar_cb(tree_t t, void *ctx)
{
   tree_t genvar = ctx;
   return tree_kind(t) == T_REF && tree_ref(t) == genvar;
}

static void elab_generate_range(tree_t r, int64_t *low, int64_t *high,
                                const elab_ctx_t *ctx)
{
   if (tree_subkind(r) == RANGE_EXPR) {
      tree_t value = tree_value(r);
      assert(tree_kind(value) == T_ATTR_REF);

      tree_t tmp = tree_new(T_ATTR_REF);
      tree_set_name(tmp, tree_name(value));
      tree_set_type(tmp, tree_type(r));
      tree_set_subkind(tmp, ATTR_LOW);

      tree_t tlow = elab_eval_expr(tmp, ctx);
      if (folded_int(tlow, low)) {
         tree_set_subkind(tmp, ATTR_HIGH);

         tree_t thigh = elab_eval_expr(tmp, ctx);
         if (folded_int(thigh, high))
            return;
      }

      error_at(tree_loc(r), "generate range is not static");
      *low = *high = 0;
   }
   else if (!folded_bounds(r, low, high)) {
      tree_t left  = elab_eval_expr(tree_left(r), ctx);
      tree_t right = elab_eval_expr(tree_right(r), ctx);

      int64_t ileft, iright;
      if (folded_int(left, &ileft) && folded_int(right, &iright)) {
         const bool asc = (tree_subkind(r) == RANGE_TO);
         *low = asc ? ileft : iright;
         *high = asc ? iright : ileft;
      }
      else {
         error_at(tree_loc(r), "generate range is not static");
         *low = *high = 0;
      }
   }
}

static void elab_for_generate(tree_t t, const elab_ctx_t *ctx)
{
   int64_t low, high;
   elab_generate_range(tree_range(t, 0), &low, &high, ctx);

   tree_t g = tree_decl(t, 0);
   assert(tree_kind(g) == T_GENERIC_DECL);

   ident_t base = tree_ident(t);

   for (int64_t i = low; i <= high; i++) {
      LOCAL_TEXT_BUF tb = tb_new();
      tb_cat(tb, istr(base));
      tb_printf(tb, "(%"PRIi64")", i);

      ident_t id = ident_new(tb_get(tb));

      tree_t b = tree_new(T_BLOCK);
      tree_set_loc(b, tree_loc(t));
      tree_set_ident(b, id);
      tree_set_loc(b, tree_loc(t));

      tree_add_stmt(ctx->out, b);

      tree_t map = tree_new(T_PARAM);
      tree_set_subkind(map, P_POS);
      tree_set_loc(map, tree_loc(g));
      tree_set_value(map, get_int_lit(g, NULL, i));

      tree_add_generic(b, g);
      tree_add_genmap(b, map);

      const char *label = istr(base);
      ident_t ninst = hpathf(ctx->inst_name, ':', "%s(%"PRIi64")", label, i);
      ident_t ndotted = ident_prefix(ctx->dotted, id, '.');

      elab_ctx_t new_ctx = {
         .out       = b,
         .inst_name = ninst,
         .dotted    = ndotted,
         .generics  = hash_new(16),
         .config    = elab_block_config(t, ctx),
      };
      elab_inherit_context(&new_ctx, ctx);

      new_ctx.prefix[0] = ident_prefix(ctx->dotted, base, '.');

      tree_t roots[] = { t };
      copy_with_renaming(roots, 1, elab_copy_genvar_cb, NULL, g, ndotted,
                         new_ctx.prefix, ARRAY_LEN(new_ctx.prefix));

      tree_t copy = roots[0];

      elab_push_scope(t, &new_ctx);
      hash_put(new_ctx.generics, g, tree_value(map));

      simplify_global(copy, new_ctx.generics, new_ctx.jit, new_ctx.registry,
                      new_ctx.mir);

      new_ctx.drivers = find_drivers(copy);

      if (error_count() == 0)
         elab_decls(copy, &new_ctx);

      if (error_count() == 0) {
         elab_lower(b, NULL, &new_ctx);
         elab_stmts(copy, &new_ctx);
      }

      elab_pop_scope(&new_ctx);
   }
}

static bool elab_generate_test(tree_t value, const elab_ctx_t *ctx)
{
   bool test;
   if (folded_bool(value, &test))
      return test;

   tree_t folded = elab_eval_expr(value, ctx);

   if (folded_bool(folded, &test))
      return test;

   error_at(tree_loc(value), "generate expression is not static");
   return false;
}

static void elab_if_generate(tree_t t, const elab_ctx_t *ctx)
{
   const int nconds = tree_conds(t);
   for (int i = 0; i < nconds; i++) {
      tree_t cond = tree_cond(t, i);
      if (!tree_has_value(cond) || elab_generate_test(tree_value(cond), ctx)) {
         tree_t b = tree_new(T_BLOCK);
         tree_set_loc(b, tree_loc(cond));
         tree_set_ident(b, tree_ident(cond));

         tree_add_stmt(ctx->out, b);

         ident_t name = tree_ident(cond);
         ident_t ninst = hpathf(ctx->inst_name, ':', "%s", name);
         ident_t ndotted = ident_prefix(ctx->dotted, name, '.');

         elab_ctx_t new_ctx = {
            .out       = b,
            .inst_name = ninst,
            .dotted    = ndotted,
            .config    = elab_block_config(cond, ctx),
         };
         elab_inherit_context(&new_ctx, ctx);

         elab_push_scope(t, &new_ctx);
         elab_decls(cond, &new_ctx);

         new_ctx.drivers = find_drivers(cond);

         if (error_count() == 0) {
            elab_lower(b, NULL, &new_ctx);
            elab_stmts(cond, &new_ctx);
         }

         elab_pop_scope(&new_ctx);
         return;
      }
   }
}

static void elab_case_generate(tree_t t, const elab_ctx_t *ctx)
{
   void *context = *mptr_get(ctx->scope->privdata);
   tree_t chosen = eval_case(ctx->jit, t, ctx->lowered, context);
   if (chosen == NULL)
      return;

   ident_t id = tree_has_ident(chosen) ? tree_ident(chosen) : tree_ident(t);

   tree_t b = tree_new(T_BLOCK);
   tree_set_loc(b, tree_loc(chosen));
   tree_set_ident(b, id);

   tree_add_stmt(ctx->out, b);

   ident_t ninst = hpathf(ctx->inst_name, ':', "%s", istr(id));
   ident_t ndotted = ident_prefix(ctx->dotted, id, '.');

   elab_ctx_t new_ctx = {
      .out       = b,
      .inst_name = ninst,
      .dotted    = ndotted,
   };
   elab_inherit_context(&new_ctx, ctx);

   elab_push_scope(t, &new_ctx);
   elab_decls(chosen, &new_ctx);

   new_ctx.drivers = find_drivers(chosen);

   if (error_count() == 0) {
      elab_lower(b, NULL, &new_ctx);
      elab_stmts(chosen, &new_ctx);
   }

   elab_pop_scope(&new_ctx);
}

static void elab_process(tree_t t, const elab_ctx_t *ctx)
{
   if (error_count() == 0)
      lower_process(ctx->lowered, t, elab_driver_set(ctx));

   tree_add_stmt(ctx->out, t);
}

static void elab_psl(tree_t t, const elab_ctx_t *ctx)
{
   if (error_count() == 0)
      psl_lower_directive(ctx->registry, ctx->lowered, ctx->cover, t);

   tree_add_stmt(ctx->out, t);
}

static void elab_verilog_stmt(tree_t wrap, const elab_ctx_t *ctx)
{
   vlog_node_t v = tree_vlog(wrap);
   switch (vlog_kind(v)) {
   case V_MOD_INST:
      {
         ident_t modname = vlog_ident2(v);
         ident_t libname = lib_name(ctx->library);

         text_buf_t *tb = tb_new();
         tb_istr(tb, libname);
         tb_append(tb, '.');
         tb_istr(tb, modname);
         tb_upcase(tb);

         ident_t qual = ident_new(tb_get(tb));

         object_t *obj = lib_get_generic(ctx->library, qual, NULL);
         if (obj == NULL) {
            error_at(vlog_loc(v), "module %s not found in library %s",
                     istr(modname), istr(libname));
            return;
         }

         vlog_node_t mod = vlog_from_object(obj);
         if (mod == NULL) {
            error_at(&obj->loc, "unit %s is not a Verilog module", istr(qual));
            return;
         }
         else if (vlog_ident2(mod) != modname) {
            diag_t *d = diag_new(DIAG_ERROR, vlog_loc(v));
            diag_printf(d, "name of Verilog module %s in library unit %s "
                        "does not match name %s in module instance %s",
                        istr(vlog_ident2(mod)), istr(qual), istr(modname),
                        istr(vlog_ident(v)));
            diag_hint(d, NULL, "this tool does not preserve case sensitivity "
                      "in module names");
            diag_emit(d);
            return;
         }

         mod_cache_t *mc = elab_cached_module(mod, ctx);

         tree_t bind = elab_verilog_binding(v, mc, ctx);
         if (bind != NULL)
            elab_verilog_module(bind, vlog_ident(v), mc, ctx);
      }
      break;
   default:
      tree_add_stmt(ctx->out, wrap);
      break;
   }
}

static void elab_stmts(tree_t t, const elab_ctx_t *ctx)
{
   const int nstmts = tree_stmts(t);
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(t, i);

      switch (tree_kind(s)) {
      case T_INSTANCE:
         elab_instance(s, ctx);
         break;
      case T_BLOCK:
         elab_block(s, ctx);
         break;
      case T_FOR_GENERATE:
         elab_for_generate(s, ctx);
         break;
      case T_IF_GENERATE:
         elab_if_generate(s, ctx);
         break;
      case T_CASE_GENERATE:
         elab_case_generate(s, ctx);
         break;
      case T_PROCESS:
         elab_process(s, ctx);
         break;
      case T_PSL_DIRECT:
         elab_psl(s, ctx);
         break;
      case T_VERILOG:
         elab_verilog_stmt(s, ctx);
         break;
      default:
         fatal_trace("unexpected statement %s", tree_kind_str(tree_kind(s)));
      }
   }
}

static void elab_block(tree_t t, const elab_ctx_t *ctx)
{
   ident_t id = tree_ident(t);

   tree_t b = tree_new(T_BLOCK);
   tree_set_ident(b, id);
   tree_set_loc(b, tree_loc(t));

   tree_add_stmt(ctx->out, b);

   ident_t ninst = hpathf(ctx->inst_name, ':', "%s", istr(id));
   ident_t ndotted = ident_prefix(ctx->dotted, id, '.');

   elab_ctx_t new_ctx = {
      .out       = b,
      .inst_name = ninst,
      .dotted    = ndotted,
      .config    = elab_block_config(t, ctx),
   };
   elab_inherit_context(&new_ctx, ctx);

   const int base_errors = error_count();

   elab_push_scope(t, &new_ctx);
   elab_generics(t, t, &new_ctx);
   elab_ports(t, t, &new_ctx);
   elab_decls(t, &new_ctx);

   if (error_count() == base_errors) {
      elab_lower(b, NULL, &new_ctx);
      elab_stmts(t, &new_ctx);
   }

   elab_pop_scope(&new_ctx);
}

static tree_t elab_top_level_binding(tree_t arch, const elab_ctx_t *ctx)
{
   tree_t bind = tree_new(T_BINDING);
   tree_set_ident(bind, tree_ident(arch));
   tree_set_loc(bind, tree_loc(arch));
   tree_set_ref(bind, arch);
   tree_set_class(bind, C_ENTITY);

   tree_t entity = tree_primary(arch);
   const int ngenerics = tree_generics(entity);

   for (int i = 0; i < ngenerics; i++) {
      tree_t g = tree_generic(entity, i);
      ident_t name = tree_ident(g);

      if (tree_flags(g) & TREE_F_PREDEFINED)
         continue;    // Predefined generic subprograms
      else if (tree_class(g) != C_CONSTANT) {
         error_at(tree_loc(g), "only constant top-level generics are "
                  "supported");
         continue;
      }

      tree_t value = elab_find_generic_override(g, ctx);
      if (value == NULL && tree_has_value(g))
         value = tree_value(g);
      else if (value == NULL) {
         error_at(tree_loc(g), "generic %s of top-level entity must have "
                  "default value or be specified using -gNAME=VALUE",
                  istr(name));
         continue;
      }

      tree_t map = tree_new(T_PARAM);
      tree_set_subkind(map, P_POS);
      tree_set_pos(map, i);
      tree_set_value(map, value);

      tree_add_genmap(bind, map);
   }

   const int nports = tree_ports(entity);
   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(entity, i);

      tree_t m = tree_new(T_PARAM);
      tree_set_subkind(m, P_POS);
      tree_set_pos(m, i);

      if (tree_has_value(p))
         tree_set_value(m, tree_value(p));
      else {
         type_t type = tree_type(p);
         if (type_is_unconstrained(type))
            error_at(tree_loc(p), "unconnected top-level port %s cannot have "
                     "unconstrained type %s", istr(tree_ident(p)),
                     type_pp(type));

         tree_t open = tree_new(T_OPEN);
         tree_set_type(open, type);
         tree_set_loc(open, tree_loc(p));

         tree_set_value(m, open);
      }

      tree_add_param(bind, m);
   }

   return bind;
}

void elab_set_generic(const char *name, const char *value)
{
   ident_t id = ident_new(name);

   for (generic_list_t *it = generic_override; it != NULL; it = it->next) {
      if (it->name == id)
         fatal("generic %s already has value '%s'", name, it->value);
   }

   generic_list_t *new = xmalloc(sizeof(generic_list_t));
   new->name  = id;
   new->value = xstrdup(value);
   new->next  = generic_override;

   generic_override = new;
}

static void elab_vhdl_root_cb(void *arg)
{
   elab_ctx_t *ctx = arg;

   tree_t vhdl = tree_from_object(ctx->root);
   assert(vhdl != NULL);

   tree_t arch, config = NULL;
   switch (tree_kind(vhdl)) {
   case T_ENTITY:
      arch = elab_pick_arch(&ctx->root->loc, vhdl, ctx);
      break;
   case T_ARCH:
      arch = vhdl;
      break;
   case T_CONFIGURATION:
      config = tree_decl(vhdl, 0);
      assert(tree_kind(config) == T_BLOCK_CONFIG);
      arch = tree_ref(config);
      break;
   default:
      fatal("%s is not a suitable top-level unit", istr(tree_ident(vhdl)));
   }

   const char *name = simple_name(istr(tree_ident2(arch)));
   ctx->inst_name = hpathf(NULL, ':', ":%s(%s)", name,
                           simple_name(istr(tree_ident(arch))));

   tree_t bind = elab_top_level_binding(arch, ctx);

   if (error_count() == 0)
      elab_architecture(bind, arch, config, ctx);
}

static void elab_verilog_root_cb(void *arg)
{
   elab_ctx_t *ctx = arg;

   vlog_node_t vlog = vlog_from_object(ctx->root);
   assert(vlog != NULL);

   mod_cache_t *mc = elab_cached_module(vlog, ctx);
   elab_verilog_module(NULL, vlog_ident2(mc->module), mc, ctx);
}

tree_t elab(object_t *top, jit_t *jit, unit_registry_t *ur, mir_context_t *mc,
            cover_data_t *cover, sdf_file_t *sdf, rt_model_t *m)
{
   make_new_arena();

   ident_t name = NULL;

   tree_t vhdl = tree_from_object(top);
   if (vhdl != NULL)
      name = ident_prefix(tree_ident(vhdl), well_known(W_ELAB), '.');

   vlog_node_t vlog = vlog_from_object(top);
   if (vlog != NULL)
      name = ident_prefix(vlog_ident(vlog), well_known(W_ELAB), '.');

   if (vhdl == NULL && vlog == NULL)
      fatal("top level is not a VHDL design unit or Verilog module");

   tree_t e = tree_new(T_ELAB);
   tree_set_ident(e, name);
   tree_set_loc(e, &(top->loc));

   lib_t work = lib_work();

   elab_ctx_t ctx = {
      .out       = e,
      .root      = top,
      .inst_name = NULL,
      .cover     = cover,
      .library   = work,
      .jit       = jit,
      .sdf       = sdf,
      .registry  = ur,
      .mir       = mc,
      .modcache  = hash_new(16),
      .dotted    = lib_name(work),
      .model     = m,
      .scope     = create_scope(m, e, NULL),
   };

   if (vhdl != NULL)
      call_with_model(m, elab_vhdl_root_cb, &ctx);
   else
      call_with_model(m, elab_verilog_root_cb, &ctx);

   const void *key;
   void *value;
   for (hash_iter_t it = HASH_BEGIN;
        hash_iter(ctx.modcache, &it, &key, &value); )
      free(value);

   hash_free(ctx.modcache);

   if (error_count() > 0)
      return NULL;

   if (opt_get_verbose(OPT_ELAB_VERBOSE, NULL))
      dump(e);

   for (generic_list_t *it = generic_override; it != NULL; it = it->next)
      warnf("generic value for %s not used", istr(it->name));

   ident_t b0_name = tree_ident(tree_stmt(e, 0));
   ident_t vu_name = ident_prefix(lib_name(ctx.library), b0_name, '.');
   unit_registry_flush(ur, vu_name);

   freeze_global_arena();
   return e;
}
