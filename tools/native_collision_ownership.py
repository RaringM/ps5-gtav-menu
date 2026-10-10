"""Bounded vehicle format reading and graph construction; no native code or reference source execution."""

from __future__ import annotations

import native_collision as collision
import native_physics as physics


def reference_check(blob, base, collision_public, physics_public, drawable_public):
    tree = physics.read(blob, base, physics.root_pointer(blob, base, physics_public), physics_public)
    drawable_pointers = sorted({p for n in tree["nodes"] if n["kind"] == "child" for p in n["external"].values() if p})
    external = []
    for index, pointer in enumerate(drawable_pointers):
        bound = collision.scalar(blob, base, pointer, drawable_public["drawable"], "BoundPointer")
        if bound:
            external.append({"drawableIndex": index, "sourcePointer": bound})
    roots = collision.roots(blob, base, {"externalCollisionLinks": external}, collision_public)
    decoded = collision.read(blob, base, roots, collision_public)
    children = {i for n in decoded["nodes"] if n["kind"] == "Composite" for i in n["links"]["children"]}
    references = [r for r in decoded["roots"] if r["owner"] == "drawable"]
    return {
        "childDrawables": len(drawable_pointers),
        "drawableBounds": len(references),
        "sharedWithCompositeChildren": sum(r["nodeIndex"] in children for r in references),
        "rootAliases": [
            {k: r[k] for k in ("owner", "ownerIndex", "nodeIndex")}
            for r in decoded["roots"]
            if r["owner"] != "drawable"
        ],
        "structuralOnly": True,
        "nativeCodeExecuted": False,
    }
