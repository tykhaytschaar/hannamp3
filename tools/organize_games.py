#!/usr/bin/env python3
"""games/ rendrakó: zipek kibontása, ROM-ok games/gb | games/gbc alá, szemét törlése.

Alapértelmezésben DRY RUN: csak kiírja, mit tenne. Tényleges futtatás: --apply.

Amit csinál:
  1. Minden .zip a games/ alatt: az egyetlen ROM-entry (.gb/.gbc) kibontása a
     kiterjesztés szerinti célkönyvtárba (games/gb vagy games/gbc), majd a zip
     törlése. Név-ütközésnél CRC+méret alapján dedup (azonos → zip törlés
     kibontás nélkül; eltérő → " (2)" utótag).
  2. Alkönyvtárakban heverő ROM-ok (pl. Featured Games, Classic Game)
     átmozgatása a célkönyvtárba, a hozzájuk tartozó mentésfájlokkal
     (azonos törzsnevű .srm/.rtc/.state*/.sav) együtt.
  3. Ismert scraper-szemét törlése (.DS_Store, gamelist*.xml*, downloaded_*,
     logs_arrm, backup), majd minden kiürült könyvtár eltávolítása.
     Ismeretlen fájlt tartalmazó könyvtárhoz NEM nyúl, csak jelzi.
"""
import argparse
import os
import sys
import zipfile
import zlib

ROM_EXTS = {".gb", ".gbc"}
JUNK_FILES = {".ds_store", "gamelist.xml", "gamelist.xml.old"}
JUNK_DIRS = {"downloaded_images", "downloaded_videos", "logs_arrm", "backup"}


def crc32_of_file(path):
    crc = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def force_op(fn, *paths):
    """fn(*paths), és ha 'Operation not permitted' (pl. macOS uchg/locked
    flag), akkor a flagek törlése után újrapróbálja."""
    try:
        fn(*paths)
    except PermissionError:
        for p in paths:
            try:
                os.chflags(p, 0)
            except OSError:
                pass
        fn(*paths)


def is_junk_file(name):
    n = name.lower()
    return n in JUNK_FILES or (n.startswith("gamelist_") and n.endswith(".xml"))


class Organizer:
    def __init__(self, root, apply_changes):
        self.root = os.path.abspath(root)
        self.apply = apply_changes
        self.dest = {".gb": os.path.join(self.root, "gb"),
                     ".gbc": os.path.join(self.root, "gbc")}
        # célnév (kisbetűs) -> (crc, size); a már meglévő és a betervezett
        # fájlok együtt, hogy dry runban is kijöjjenek az ütközések
        self.planned = {}
        self.stats = {"extracted": 0, "dedup": 0, "renamed": 0, "moved": 0,
                      "saves": 0, "zip_deleted": 0, "junk": 0, "rmdir": 0,
                      "skipped": 0}
        self.warnings = []

    def log(self, action, detail):
        prefix = "" if self.apply else "[DRY] "
        print(f"{prefix}{action:9s} {detail}")

    def warn(self, msg):
        self.warnings.append(msg)
        print(f"FIGYELEM: {msg}", file=sys.stderr)

    # --- ütközéskezelés -------------------------------------------------
    def existing_sig(self, path):
        return (crc32_of_file(path), os.path.getsize(path))

    def claim_target(self, dest_dir, name, crc, size):
        """Cél-útvonal foglalása. None = azonos tartalom már megvan (dedup)."""
        base, ext = os.path.splitext(name)
        candidate = name
        n = 1
        while True:
            target = os.path.join(dest_dir, candidate)
            key = target.lower()
            if key in self.planned:
                sig = self.planned[key]
            elif os.path.exists(target):
                sig = self.existing_sig(target)
                self.planned[key] = sig
            else:
                self.planned[key] = (crc, size)
                return target, n > 1
            if sig == (crc, size):
                return None, False  # tartalomra azonos duplikátum
            n += 1
            candidate = f"{base} ({n}){ext}"

    # --- 1. zipek --------------------------------------------------------
    def process_zips(self):
        zips = []
        for dirpath, _dirs, files in os.walk(self.root):
            zips += [os.path.join(dirpath, f) for f in files
                     if f.lower().endswith(".zip")]
        for zpath in sorted(zips):
            rel = os.path.relpath(zpath, self.root)
            try:
                with zipfile.ZipFile(zpath) as zf:
                    infos = [i for i in zf.infolist() if not i.is_dir()]
                    exts = {os.path.splitext(i.filename)[1].lower() for i in infos}
                    if len(infos) != 1 or not exts <= ROM_EXTS:
                        self.stats["skipped"] += 1
                        self.warn(f"kihagyva (nem 1 ROM van benne): {rel} "
                                  f"[{[i.filename for i in infos]}]")
                        continue
                    info = infos[0]
                    ext = exts.pop()
                    name = os.path.basename(info.filename)
                    target, renamed = self.claim_target(
                        self.dest[ext], name, info.CRC, info.file_size)
                    if target is None:
                        self.stats["dedup"] += 1
                        self.log("DEDUP", f"{rel} — azonos tartalom már megvan, "
                                          "zip törlése kibontás nélkül")
                    else:
                        if renamed:
                            self.stats["renamed"] += 1
                        self.stats["extracted"] += 1
                        self.log("KIBONT", f"{rel} -> "
                                 f"{os.path.relpath(target, self.root)}")
                        if self.apply:
                            tmp = target + ".part"
                            with zf.open(info) as src, open(tmp, "wb") as dst:
                                while True:
                                    chunk = src.read(1 << 20)
                                    if not chunk:
                                        break
                                    dst.write(chunk)
                            # a zipfile olvasáskor CRC-t ellenőriz; ha idáig
                            # eljutottunk, az adat hibátlan
                            os.replace(tmp, target)
            except (zipfile.BadZipFile, zlib.error, OSError) as e:
                self.stats["skipped"] += 1
                self.warn(f"hibás zip, kihagyva: {rel} ({e})")
                continue
            self.stats["zip_deleted"] += 1
            if self.apply:
                force_op(os.remove, zpath)

    # --- 2. heverő ROM-ok ------------------------------------------------
    def move_loose_roms(self):
        for dirpath, _dirs, files in os.walk(self.root):
            for f in sorted(files):
                ext = os.path.splitext(f)[1].lower()
                if ext not in ROM_EXTS:
                    continue
                if os.path.realpath(dirpath) == os.path.realpath(self.dest[ext]):
                    continue  # már jó helyen van
                src = os.path.join(dirpath, f)
                rel = os.path.relpath(src, self.root)
                crc, size = self.existing_sig(src)
                target, renamed = self.claim_target(self.dest[ext], f, crc, size)
                if target is None:
                    self.stats["dedup"] += 1
                    self.log("DEDUP", f"{rel} — azonos tartalom már megvan, "
                                      "törlés")
                    if self.apply:
                        force_op(os.remove, src)
                    continue
                if renamed:
                    self.stats["renamed"] += 1
                self.stats["moved"] += 1
                self.log("MOZGAT", f"{rel} -> "
                         f"{os.path.relpath(target, self.root)}")
                if self.apply:
                    force_op(os.rename, src, target)
                self.move_companions(dirpath, f, os.path.dirname(target),
                                     os.path.basename(target))

    def move_companions(self, src_dir, rom_name, dest_dir, new_rom_name):
        """A ROM mellé tartozó mentésfájlok (azonos törzsnév) mozgatása."""
        stem = os.path.splitext(rom_name)[0]
        new_stem = os.path.splitext(new_rom_name)[0]
        for f in sorted(os.listdir(src_dir) if os.path.isdir(src_dir) else []):
            if not f.startswith(stem + "."):
                continue
            ext = f[len(stem):]
            if os.path.splitext(f)[1].lower() in ROM_EXTS | {".zip"}:
                continue
            src = os.path.join(src_dir, f)
            target = os.path.join(dest_dir, new_stem + ext)
            if os.path.exists(target):
                self.warn(f"mentésfájl-ütközés, marad: "
                          f"{os.path.relpath(src, self.root)}")
                continue
            self.stats["saves"] += 1
            self.log("MENTÉS", f"{os.path.relpath(src, self.root)} -> "
                     f"{os.path.relpath(target, self.root)}")
            if self.apply:
                force_op(os.rename, src, target)

    # --- 3. takarítás ----------------------------------------------------
    def cleanup(self):
        # dry runban nyilvántartjuk, mit "töröltünk" volna, hogy az
        # üres-könyvtár-számítás is stimmeljen
        removed = set()

        def gone(path):
            return self.apply is False and path in removed

        for dirpath, _dirs, files in os.walk(self.root, topdown=False):
            for f in files:
                p = os.path.join(dirpath, f)
                if gone(p):
                    continue
                keep = True
                base = os.path.basename(dirpath).lower()
                if is_junk_file(f):
                    keep = False
                elif base in JUNK_DIRS or self.under_junk_dir(dirpath):
                    keep = False
                if not keep:
                    self.stats["junk"] += 1
                    self.log("TÖRÖL", os.path.relpath(p, self.root))
                    if self.apply:
                        force_op(os.remove, p)
                    else:
                        removed.add(p)
            if dirpath == self.root or \
               os.path.realpath(dirpath) in (os.path.realpath(self.dest[".gb"]),
                                             os.path.realpath(self.dest[".gbc"])):
                continue
            # kiürült könyvtár?
            leftover = []
            for entry in os.listdir(dirpath):
                p = os.path.join(dirpath, entry)
                if gone(p):
                    continue
                # dry runban: zip/ROM, amit "elvittünk" volna, szintén nem marad
                if not self.apply and self.would_vanish(p):
                    continue
                leftover.append(entry)
            if leftover:
                self.warn(f"nem üres, marad: "
                          f"{os.path.relpath(dirpath, self.root)}/ "
                          f"(pl. {leftover[:3]})")
            else:
                self.stats["rmdir"] += 1
                self.log("RMDIR", os.path.relpath(dirpath, self.root) + "/")
                if self.apply:
                    force_op(os.rmdir, dirpath)
                else:
                    removed.add(dirpath)

    def under_junk_dir(self, dirpath):
        rel = os.path.relpath(dirpath, self.root)
        return any(part.lower() in JUNK_DIRS for part in rel.split(os.sep))

    def would_vanish(self, path):
        """Dry run: ez a fájl/könyvtár eltűnne-e az apply futás végére."""
        if os.path.isdir(path):
            return all(self.would_vanish(os.path.join(path, e))
                       for e in os.listdir(path))
        name = os.path.basename(path)
        ext = os.path.splitext(name)[1].lower()
        dirname = os.path.dirname(path)
        if ext == ".zip":
            return True  # minden feldolgozott zip törlődik (a kihagyottak nem!)
        if ext in ROM_EXTS and \
           os.path.realpath(dirname) != os.path.realpath(self.dest[ext]):
            return True
        if is_junk_file(name) or self.under_junk_dir(path):
            return True
        # mentésfájl, aminek a ROM-ja elköltözik ebből a könyvtárból
        for e in os.listdir(dirname):
            e_ext = os.path.splitext(e)[1].lower()
            if e_ext in ROM_EXTS and name.startswith(os.path.splitext(e)[0] + ".") \
               and os.path.realpath(dirname) != os.path.realpath(self.dest[e_ext]):
                return True
        return False

    # ----------------------------------------------------------------------
    def run(self):
        for d in self.dest.values():
            if not os.path.isdir(d):
                if self.apply:
                    os.makedirs(d)
        self.process_zips()
        self.move_loose_roms()
        self.cleanup()
        mode = "APPLY" if self.apply else "DRY RUN"
        print(f"\n===== ÖSSZEGZÉS ({mode}) =====")
        s = self.stats
        print(f"  zipből kibontva:        {s['extracted']}")
        print(f"  zip törölve:            {s['zip_deleted']}")
        print(f"  duplikátum (eldobva):   {s['dedup']}")
        print(f"  átnevezve ütközés miatt:{s['renamed']}")
        print(f"  heverő ROM átmozgatva:  {s['moved']}")
        print(f"  mentésfájl átmozgatva:  {s['saves']}")
        print(f"  szemétfájl törölve:     {s['junk']}")
        print(f"  könyvtár eltávolítva:   {s['rmdir']}")
        print(f"  kihagyott zip:          {s['skipped']}")
        if self.warnings:
            print(f"  figyelmeztetés:         {len(self.warnings)} (lásd stderr)")
        return 0 if not self.warnings else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("games_dir", nargs="?",
                    default=os.path.join(os.path.dirname(os.path.dirname(
                        os.path.abspath(__file__))), "games"),
                    help="a games könyvtár (alapértelmezés: <repo>/games)")
    ap.add_argument("--apply", action="store_true",
                    help="tényleges végrehajtás (enélkül dry run)")
    args = ap.parse_args()
    if not os.path.isdir(args.games_dir):
        ap.error(f"nincs ilyen könyvtár: {args.games_dir}")
    sys.exit(Organizer(args.games_dir, args.apply).run())


if __name__ == "__main__":
    main()
