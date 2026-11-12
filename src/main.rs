//! hd2tcg: texture cache files (.DX1 .DX2 .DX3 .565 .555) for Hidden & Dangerous 2's unmodified
//! LS3DF.dll, made without the game.

mod datafs;
mod image;
mod model4ds;
mod s3tc;
mod texture;

use std::collections::{BTreeSet, HashSet};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;

use datafs::{lower, DataFs};
use model4ds::{read_model_textures, DriverStates};
use s3tc::Encoder;
use texture::*;

const USAGE: &str = "\
hd2tcg - texture cache files for Hidden & Dangerous 2 (unmodified LS3DF.dll)

  hd2tcg generate [options] <model.4ds | folder>...
      The cache files the models' materials make the game write, next to their images.
  hd2tcg texture [options] <name> [--alpha <name>] --flags <flags>
      The cache file of one IGraph::LoadTexture call (textures the game loads itself).
  hd2tcg verify [options] <cache file | folder | @list.txt>...
      Remakes existing cache files from their images and compares them byte for byte.
  hd2tcg info <cache file>...
      Shows a cache file's header.

Options:
  --data <dir>        A folder standing for the game folder (with Maps, Maps_C, Maps_U);
                      repeat for more, the first that has a file wins (put a mod folder
                      before the game's unpacked data). Default: the current folder.
  --out <dir>         Write cache files under <dir> instead of next to their images.
  --all-roots         generate/texture: also write caches for images found in the second
                      and later --data folders (default: only the first).
  --half both|on|off  The \"half size textures\" setting the caches are for (default both,
                      as the game's own caches).
  --no-compress       Texture compression off: 16-bit caches instead of DXT.
  --no-dither         Dithering off (the game never turns it off).
  --fmt16 565|555     The adapter's 16-bit texture format (default 565).
  --max-texture <n>   The adapter's maximum texture size (default 16384).
  --carry-encoder     Do not reset the S3TC encoder between textures (it carries state over
                      from texture to texture, as within one game session).
  --dry-run           List the cache files, write nothing.
  --jobs <n>          Threads (default: all cores).
  --report <file>     verify: a tab-separated line per cache file.
  -v                  More output.

Flags for 'texture': a hex number or a list of names joined by ',' or '|':
  source_alpha use_alpha alpha_1bit (colorkey) dither mipmap compress truecolor half noresize
";

struct Options {
    data: Vec<PathBuf>,
    out: Option<PathBuf>,
    all_roots: bool,
    halves: Vec<bool>,
    states: DriverStates,
    device: Device,
    carry_encoder: bool,
    dry_run: bool,
    jobs: usize,
    report: Option<PathBuf>,
    verbose: bool,
    alpha: String,
    flags: Option<String>,
    args: Vec<String>,
}

fn usage(error: Option<&str>) -> ! {
    match error {
        Some(e) => {
            eprintln!("hd2tcg: {e}\n");
            eprint!("{USAGE}");
            std::process::exit(2);
        }
        None => {
            print!("{USAGE}");
            std::process::exit(0);
        }
    }
}

fn parse_flags(text: &str) -> Option<u32> {
    if text.is_empty() {
        return None;
    }
    let hex = text.trim_start_matches("0x").trim_start_matches("0X");
    if let Ok(v) = u32::from_str_radix(hex, 16) {
        return Some(v);
    }
    let mut flags = 0;
    for word in text.split([',', '|', '+']) {
        flags |= match lower(word).as_str() {
            "source_alpha" => ITC_SOURCE_ALPHA,
            "use_alpha" => ITC_USE_ALPHA,
            "alpha_1bit" | "colorkey" => ITC_ALPHA_1BIT,
            "dither" => ITC_DITHER,
            "mipmap" => ITC_MIPMAP,
            "compress" => ITC_COMPRESS,
            "truecolor" => ITC_TRUECOLOR,
            "half" => ITC_HALF,
            "noresize" => ITC_NORESIZE,
            "0" => 0,
            _ => return None,
        };
    }
    Some(flags)
}

fn flag_names(f: u32) -> String {
    const NAMES: [(u32, &str); 11] = [
        (ITC_SOURCE_ALPHA, "source_alpha"),
        (ITC_USE_ALPHA, "use_alpha"),
        (ITC_ALPHA_1BIT, "alpha_1bit"),
        (ITC_STREAMED, "streamed"),
        (ITC_DITHER, "dither"),
        (ITC_MIPMAP, "mipmap"),
        (ITC_COMPRESS, "compress"),
        (ITC_TRUECOLOR, "truecolor"),
        (ITC_KEEP_PIXELS, "keep_pixels"),
        (ITC_HALF, "half"),
        (ITC_NORESIZE, "noresize"),
    ];
    let s: Vec<&str> = NAMES.iter().filter(|n| f & n.0 != 0).map(|n| n.1).collect();
    if s.is_empty() {
        "0".into()
    } else {
        s.join("|")
    }
}

fn parse_args(argv: &[String]) -> Options {
    let mut o = Options {
        data: Vec::new(),
        out: None,
        all_roots: false,
        halves: vec![false, true],
        states: DriverStates::default(),
        device: Device::default(),
        carry_encoder: false,
        dry_run: false,
        jobs: 0,
        report: None,
        verbose: false,
        alpha: String::new(),
        flags: None,
        args: Vec::new(),
    };
    let mut it = argv.iter();
    while let Some(a) = it.next() {
        let mut value = || it.next().cloned().unwrap_or_else(|| usage(Some(&format!("{a} needs a value"))));
        match a.as_str() {
            "--data" => o.data.push(PathBuf::from(value())),
            "--out" => o.out = Some(PathBuf::from(value())),
            "--all-roots" => o.all_roots = true,
            "--half" => {
                o.halves = match value().as_str() {
                    "both" => vec![false, true],
                    "on" => vec![true],
                    "off" => vec![false],
                    _ => usage(Some("--half takes both, on or off")),
                }
            }
            "--no-compress" => o.states.compress = false,
            "--no-dither" => o.states.dither = false,
            "--fmt16" => {
                o.device.fmt16 = match value().as_str() {
                    "565" => Format::R5G6B5,
                    "555" => Format::X1R5G5B5,
                    _ => usage(Some("--fmt16 takes 565 or 555")),
                }
            }
            "--max-texture" => {
                let n: u32 = value().parse().unwrap_or(0);
                if n < 4 {
                    usage(Some("--max-texture must be at least 4"));
                }
                o.device.max_width = n;
                o.device.max_height = n;
            }
            "--carry-encoder" => o.carry_encoder = true,
            "--dry-run" => o.dry_run = true,
            "--jobs" => o.jobs = value().parse().unwrap_or(0),
            "--report" => o.report = Some(PathBuf::from(value())),
            "--alpha" => o.alpha = value(),
            "--flags" => o.flags = Some(value()),
            "-v" | "--verbose" => o.verbose = true,
            "-h" | "--help" => usage(None),
            s if s.len() > 1 && s.starts_with('-') => usage(Some(&format!("unknown option {s}"))),
            _ => o.args.push(a.clone()),
        }
    }
    if o.data.is_empty() {
        o.data.push(PathBuf::from("."));
    }
    if o.carry_encoder {
        o.jobs = 1; // one encoder state, in a fixed order
    }
    if o.jobs == 0 {
        o.jobs = std::thread::available_parallelism().map_or(1, |n| n.get());
    }
    o
}

/// Runs `f(i, encoder)` for i in [0, n) on `jobs` threads, each with its own encoder state.
fn parallel_for<F>(n: usize, jobs: usize, f: F)
where
    F: Fn(usize, &mut Encoder) + Sync,
{
    let next = AtomicUsize::new(0);
    std::thread::scope(|s| {
        for _ in 0..jobs.min(n).max(1) {
            s.spawn(|| {
                let mut encoder = Box::new(Encoder::new());
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    if i >= n {
                        break;
                    }
                    f(i, &mut encoder);
                }
            });
        }
    });
}

/// The encoder a texture starts from: a fresh DLL's, or the thread's running one.
fn encoder_for<'a>(carry: bool, running: &'a mut Encoder, fresh: &'a mut Option<Box<Encoder>>) -> &'a mut Encoder {
    if carry {
        running
    } else {
        fresh.insert(Box::new(Encoder::new()))
    }
}

fn collect_files(args: &[String], exts: &[&str]) -> Vec<PathBuf> {
    let wanted = |p: &Path| p.extension().is_some_and(|e| exts.contains(&lower(&e.to_string_lossy()).as_str()));
    let mut files = Vec::new();
    fn walk(dir: &Path, wanted: &dyn Fn(&Path) -> bool, out: &mut Vec<PathBuf>) {
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(&p, wanted, out);
            } else if wanted(&p) {
                out.push(p);
            }
        }
    }
    for a in args {
        if let Some(list) = a.strip_prefix('@').filter(|l| !l.is_empty()) {
            match std::fs::read_to_string(list) {
                Ok(text) => {
                    for line in text.lines().map(str::trim).filter(|l| !l.is_empty()) {
                        let p = PathBuf::from(line);
                        if p.is_file() && wanted(&p) {
                            files.push(p);
                        }
                    }
                }
                Err(_) => eprintln!("hd2tcg: cannot read {list}"),
            }
            continue;
        }
        let p = PathBuf::from(a);
        if p.is_dir() {
            walk(&p, &wanted, &mut files);
        } else if p.is_file() {
            files.push(p);
        } else {
            eprintln!("hd2tcg: not found: {a}");
        }
    }
    files.sort();
    files
}

/// A FILETIME as a UTC date.
fn show_time(t: u64) -> String {
    if t == 0 {
        return "0".into();
    }
    let secs = (t / 10_000_000) as i64 - 11_644_473_600;
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02} UTC", rem / 3600, rem / 60 % 60, rem % 60)
}

// ---------------------------------------------------------------- generate / texture

struct Job {
    load: TextureLoad,
    /// The model (and material) asking for it.
    source: String,
}

fn run_loads(o: &Options, jobs: Vec<Job>) -> u8 {
    let data = DataFs::new(&o.data);
    // One cache per distinct load.
    let mut seen = HashSet::new();
    let jobs: Vec<Job> = jobs
        .into_iter()
        .filter(|j| seen.insert(format!("{}|{}|{}", lower(&j.load.name), lower(&j.load.alpha_name), j.load.flags)))
        .collect();

    let results: Vec<Mutex<Option<CacheResult>>> = jobs.iter().map(|_| Mutex::new(None)).collect();
    parallel_for(jobs.len(), o.jobs, |i, running| {
        let mut fresh = None;
        let enc = encoder_for(o.carry_encoder, running, &mut fresh);
        let mut r = build_cache(&jobs[i].load, &o.device, &data, enc, BuildOptions { contents: false });
        let root = r.image_file.as_deref().and_then(|f| data.root_of(f));
        if r.outcome == Outcome::Cache && !o.dry_run && (root == Some(0) || o.all_roots) {
            r = build_cache(&jobs[i].load, &o.device, &data, enc, BuildOptions { contents: true });
        }
        *results[i].lock().unwrap() = Some(r);
    });

    let (mut written, mut unchanged, mut skipped_root, mut no_cache, mut errors, mut planned) = (0, 0, 0, 0, 0, 0);
    let mut written_paths = HashSet::new();
    let mut warned = HashSet::new();
    let root0 = data.roots()[0].display().to_string();
    for (job, cell) in jobs.iter().zip(&results) {
        let r = cell.lock().unwrap().take().unwrap();
        for w in &r.warnings {
            if warned.insert(w.clone()) {
                println!("warning: {w}");
            }
        }
        match r.outcome {
            Outcome::Error => {
                errors += 1;
                println!("error: {} ({}): {}", job.load.name, job.source, r.message);
                continue;
            }
            Outcome::NoCache => {
                no_cache += 1;
                if o.verbose {
                    println!("no cache: {}: {}", job.load.name, r.message);
                }
                continue;
            }
            Outcome::Cache => {}
        }
        let root = r.image_file.as_deref().and_then(|f| data.root_of(f)).unwrap_or(usize::MAX);
        // Textures of the first root only, unless asked: the others come with their own caches.
        if root != 0 && !o.all_roots {
            skipped_root += 1;
            if o.verbose {
                println!("skip (not in {root0}): {}", r.cache_path);
            }
            continue;
        }
        let base = match &o.out {
            Some(out) => out.clone(),
            None => data.roots()[root].clone(),
        };
        let dest = base.join(&r.cache_path);
        if !written_paths.insert(lower(&dest.to_string_lossy())) {
            continue;
        }
        if o.dry_run {
            planned += 1;
            println!(
                "{:<48} {} {}x{}, {} level{}, {}  <- {}{}{}",
                r.cache_path,
                r.format.name(),
                r.width,
                r.height,
                r.levels,
                if r.levels == 1 { "" } else { "s" },
                flag_names(r.flags),
                r.image_path,
                if r.alpha_path.is_empty() { "" } else { " + " },
                r.alpha_path
            );
            continue;
        }
        if std::fs::read(&dest).is_ok_and(|old| old == r.bytes) {
            unchanged += 1;
            if o.verbose {
                println!("unchanged: {}", dest.display());
            }
            continue;
        }
        if let Some(parent) = dest.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        if std::fs::write(&dest, &r.bytes).is_err() {
            errors += 1;
            println!("error: cannot write {}", dest.display());
            continue;
        }
        written += 1;
        println!(
            "wrote {} ({} {}x{}, {} level{})",
            dest.display(),
            r.format.name(),
            r.width,
            r.height,
            r.levels,
            if r.levels == 1 { "" } else { "s" }
        );
    }
    // Images of the first root that the game will not load because the same name is found first
    // in another search directory (or another root).
    let names: BTreeSet<&str> = jobs.iter().map(|j| j.load.name.as_str()).collect();
    for n in names.into_iter().filter(|n| !n.contains('\\')) {
        let Some(used) = data.find_texture(n) else { continue };
        let Some(used_file) = data.locate(&used) else { continue };
        for dir in &data.search_dirs {
            let own = data.roots()[0].join(format!("{dir}\\{n}"));
            if own.is_file() && lower(&own.to_string_lossy()) != lower(&used_file.to_string_lossy()) {
                println!(
                    "warning: {} is not used: the game finds {used} first ({})",
                    own.display(),
                    used_file.display()
                );
                break;
            }
        }
    }
    if o.dry_run {
        print!("\n{planned} cache file{}", if planned == 1 { "" } else { "s" });
    } else {
        print!("\n{written} written, {unchanged} unchanged");
    }
    println!(
        ", {no_cache} without a cache file, {skipped_root} skipped (images outside {root0}), {errors} error{}",
        if errors == 1 { "" } else { "s" }
    );
    u8::from(errors > 0)
}

fn cmd_generate(o: &Options) -> u8 {
    if o.args.is_empty() {
        usage(Some("generate needs model files or folders"));
    }
    let models = collect_files(&o.args, &["4ds"]);
    let mut jobs = Vec::new();
    let mut bad = false;
    for m in &models {
        for (k, &half) in o.halves.iter().enumerate() {
            let st = DriverStates { half, ..o.states };
            match read_model_textures(m, &st) {
                Ok(loads) => {
                    let file = m.file_name().map(|f| f.to_string_lossy().into_owned()).unwrap_or_default();
                    for l in loads {
                        jobs.push(Job {
                            source: format!(
                                "{file} material {}{}",
                                l.material + 1,
                                if l.environment { " (environment)" } else { "" }
                            ),
                            load: l.load,
                        });
                    }
                }
                Err(e) => {
                    if k == 0 {
                        println!("warning: {}: {e}", m.display());
                        bad = true;
                    }
                }
            }
        }
    }
    println!(
        "{} model file{}, {} texture load{}",
        models.len(),
        if models.len() == 1 { "" } else { "s" },
        jobs.len(),
        if jobs.len() == 1 { "" } else { "s" }
    );
    run_loads(o, jobs) | u8::from(bad)
}

fn cmd_texture(o: &Options) -> u8 {
    if o.args.len() != 1 {
        usage(Some("texture takes one texture name"));
    }
    let Some(text) = &o.flags else { usage(Some("texture needs --flags")) };
    let Some(flags) = parse_flags(text) else { usage(Some("bad --flags")) };
    let job = Job {
        load: TextureLoad { name: o.args[0].clone(), alpha_name: o.alpha.clone(), flags },
        source: "command line".into(),
    };
    run_loads(o, vec![job])
}

// ---------------------------------------------------------------- verify

/// The texture loads that could have written a cache file of this game-relative name.
fn candidates(rel: &str, data: &DataFs) -> Vec<TextureLoad> {
    let mut out = Vec::new();
    let Some(dot) = rel.rfind('.') else { return out };
    let ext = lower(&rel[dot + 1..]);
    let stem = &rel[..dot];
    let Some(us) = stem.rfind('_') else { return out };
    let mut tail = &stem[us + 1..];
    let half = tail.ends_with(['L', 'l']);
    if half {
        tail = &tail[..tail.len() - 1];
    }
    if tail.is_empty() || !tail.bytes().all(|b| b.is_ascii_digit()) {
        return out;
    }
    let levels: u32 = tail.parse().unwrap_or(0);
    let base = &stem[..us];
    let common = if levels > 1 { ITC_MIPMAP } else { 0 } | if half { ITC_HALF } else { 0 };
    // The images that could have this name (a texture may exist as both .bmp and .tga).
    let images = |b: &str| -> Vec<String> {
        [".bmp", ".tga"].iter().map(|e| format!("{b}{e}")).filter(|p| data.locate(p).is_some()).collect()
    };
    let mut add = |img: &str, alpha: &str, flags: u32| {
        out.push(TextureLoad { name: img.to_string(), alpha_name: alpha.to_string(), flags });
    };
    let dxt = ITC_COMPRESS | ITC_DITHER | common;
    match ext.as_str() {
        "dx1" | "dx2" => {
            let f = dxt | if ext == "dx2" { ITC_ALPHA_1BIT } else { 0 };
            for img in images(base) {
                add(&img, "", f);
                add(&img, "", f & !ITC_DITHER);
            }
        }
        "dx3" => {
            // "<image>_<opacity map>" or just "<image>".
            let start = base.rfind('\\').map_or(0, |i| i + 1);
            for (p, _) in base.char_indices().filter(|&(p, c)| c == '_' && p >= start) {
                let alpha_base = &base[p + 1..];
                for img in images(&base[..p]) {
                    for e in [".bmp", ".tga"] {
                        let alpha = format!("{alpha_base}{e}");
                        if data.find_texture(&alpha).is_some() {
                            add(&img, &alpha, dxt | ITC_USE_ALPHA);
                        }
                    }
                }
            }
            for img in images(base) {
                add(&img, "", dxt | ITC_SOURCE_ALPHA);
                add(&img, "", dxt | ITC_ALPHA_1BIT | ITC_MIPMAP);
                add(&img, "", dxt | ITC_SOURCE_ALPHA | ITC_ALPHA_1BIT | ITC_MIPMAP);
            }
        }
        "565" | "555" => {
            let f = common | if ext == "555" { ITC_ALPHA_1BIT } else { 0 };
            for img in images(base) {
                for extra in [0, ITC_SOURCE_ALPHA] {
                    add(&img, "", f | extra);
                    add(&img, "", f | extra | ITC_DITHER);
                }
            }
        }
        _ => {}
    }
    out
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
enum Verdict {
    Exact,
    SameData,
    Differs,
    #[default]
    Unresolved,
    Error,
}

#[derive(Default, Clone)]
struct VerifyResult {
    verdict: Verdict,
    detail: String,
    flags: u32,
    /// Blocks (DXT) or pixels (16-bit).
    units: usize,
    units_differing: usize,
}

fn verify_one(file: &Path, data: &DataFs, o: &Options, running: &mut Encoder) -> VerifyResult {
    let err = |d: &str| VerifyResult { verdict: Verdict::Error, detail: d.into(), ..Default::default() };
    let Some(rel) = data.relative(file) else { return err("not inside a --data folder") };
    let Ok(shipped) = std::fs::read(file) else { return err("cannot read") };
    let ext = lower(rel.rsplit('.').next().unwrap_or(""));
    let dxt3 = ext == "dx3";
    let Some(h) = parse_cache_header(&shipped, dxt3) else { return err("not a cache file") };
    if &h.magic != if ext.starts_with('d') { b"DXT\0" } else { b"TEX\0" } {
        return err("not a cache file");
    }
    let cands = candidates(&rel, data);
    let mut best = VerifyResult {
        detail: if cands.is_empty() { "no image for this name" } else { "no texture load makes this name" }.into(),
        ..Default::default()
    };
    let mut any = false;
    for c in &cands {
        let mut fresh = None;
        let enc = encoder_for(o.carry_encoder, running, &mut fresh);
        let r = build_cache(c, &o.device, data, enc, BuildOptions { contents: true });
        if r.outcome != Outcome::Cache || lower(&r.cache_path) != lower(&rel) {
            if r.outcome == Outcome::Error && !any {
                best.detail = r.message;
            }
            continue;
        }
        any = true;
        let g = parse_cache_header(&r.bytes, dxt3).unwrap();
        let same_size = r.bytes.len() == shipped.len();
        let same_data = same_size && r.bytes[g.data_offset..] == shipped[h.data_offset..];
        let same_dims = g.width == h.width && g.height == h.height && g.version == h.version;
        let mut cur = VerifyResult { flags: r.flags, ..Default::default() };
        if same_data && same_dims {
            let same_times = g.time == h.time && g.alpha_time == h.alpha_time;
            cur.verdict = if same_times { Verdict::Exact } else { Verdict::SameData };
            if !same_times {
                cur.detail = format!("image time {} in the file, {} on disk", show_time(h.time), show_time(g.time));
            }
        } else if !same_size || !same_dims {
            cur.verdict = Verdict::Differs;
            cur.detail = format!(
                "size: {}x{}, {} bytes in the file; made {}x{}, {} bytes",
                h.width,
                h.height,
                shipped.len(),
                g.width,
                g.height,
                r.bytes.len()
            );
            cur.units = 1;
            cur.units_differing = 1;
        } else {
            let unit = match ext.as_str() {
                "dx1" | "dx2" => 8,
                "dx3" => 16,
                _ => 2,
            };
            let a = &shipped[h.data_offset..];
            let b = &r.bytes[g.data_offset..];
            let n = a.len() / unit;
            let diff = (0..n).filter(|&k| a[k * unit..(k + 1) * unit] != b[k * unit..(k + 1) * unit]).count();
            cur.verdict = Verdict::Differs;
            cur.units = n;
            cur.units_differing = diff;
            cur.detail = format!("{diff} of {n} {} differ", if unit == 2 { "pixels" } else { "blocks" });
        }
        let better = best.verdict == Verdict::Unresolved
            || cur.verdict < best.verdict
            || (cur.verdict == Verdict::Differs
                && best.verdict == Verdict::Differs
                && cur.units_differing * best.units < best.units_differing * cur.units);
        if better {
            best = cur;
        }
        if best.verdict == Verdict::Exact {
            break;
        }
    }
    best
}

fn cmd_verify(o: &Options) -> u8 {
    if o.args.is_empty() {
        usage(Some("verify needs cache files or folders"));
    }
    let data = DataFs::new(&o.data);
    let files = collect_files(&o.args, &["dx1", "dx2", "dx3", "565", "555"]);
    let results: Vec<Mutex<VerifyResult>> = files.iter().map(|_| Mutex::new(VerifyResult::default())).collect();
    let done = AtomicUsize::new(0);
    let print = Mutex::new(());
    const NAMES: [&str; 5] = ["exact", "same data", "DIFFERS", "unresolved", "ERROR"];
    parallel_for(files.len(), o.jobs, |i, running| {
        let v = verify_one(&files[i], &data, o, running);
        let d = done.fetch_add(1, Ordering::Relaxed) + 1;
        {
            let _lock = print.lock().unwrap();
            if o.verbose || matches!(v.verdict, Verdict::Differs | Verdict::Error) {
                println!("{:<10} {}  {}", NAMES[v.verdict as usize], files[i].display(), v.detail);
            } else if d % 1000 == 0 {
                eprintln!("  {d} / {}", files.len());
            }
        }
        *results[i].lock().unwrap() = v;
    });

    let results: Vec<VerifyResult> = results.into_iter().map(|m| m.into_inner().unwrap()).collect();
    let mut counts = [0usize; 5];
    let (mut units, mut units_diff) = (0, 0);
    for v in &results {
        counts[v.verdict as usize] += 1;
        if v.verdict == Verdict::Differs {
            units += v.units;
            units_diff += v.units_differing;
        }
    }
    if let Some(report) = &o.report {
        const KEYS: [&str; 5] = ["exact", "same_data", "differs", "unresolved", "error"];
        let mut text = String::from("file\tverdict\tflags\tdetail\n");
        for (f, v) in files.iter().zip(&results) {
            let flags = if v.verdict <= Verdict::Differs { flag_names(v.flags) } else { String::new() };
            text.push_str(&format!("{}\t{}\t{}\t{}\n", f.display(), KEYS[v.verdict as usize], flags, v.detail));
        }
        if std::fs::write(report, text).is_err() {
            eprintln!("hd2tcg: cannot write {}", report.display());
        }
    }
    println!(
        "\n{} cache files: {} identical, {} identical but for the image time stamps, {} differ ({} of {} blocks/pixels), {} unresolved, {} errors",
        files.len(),
        counts[0],
        counts[1],
        counts[2],
        units_diff,
        units,
        counts[3],
        counts[4]
    );
    u8::from(counts[2] > 0 || counts[4] > 0)
}

fn cmd_info(o: &Options) -> u8 {
    if o.args.is_empty() {
        usage(Some("info needs cache files"));
    }
    let mut bad = false;
    for file in &o.args {
        let ext = lower(Path::new(file).extension().map(|e| e.to_string_lossy().into_owned()).unwrap_or_default().as_str());
        let Some((bytes, h)) =
            std::fs::read(file).ok().and_then(|b| parse_cache_header(&b, ext == "dx3").map(|h| (b, h)))
        else {
            println!("{file}: not a cache file");
            bad = true;
            continue;
        };
        print!(
            "{file}\n  magic {}, version {}, {}x{}, image time {}",
            String::from_utf8_lossy(&h.magic[..3]),
            h.version,
            h.width,
            h.height,
            show_time(h.time)
        );
        if h.has_alpha_time {
            print!(", opacity map time {}", show_time(h.alpha_time));
        }
        let (mut pos, mut w, mut ht, mut levels) = (h.data_offset, h.width, h.height, 0);
        while pos < bytes.len() && w > 0 && ht > 0 {
            let (bw, bh) = (w.div_ceil(4) as usize, ht.div_ceil(4) as usize);
            pos += match ext.as_str() {
                "dx1" | "dx2" => bw * bh * 8,
                "dx3" => bw * bh * 16,
                _ => pitch16(w) as usize * ht as usize,
            };
            levels += 1;
            if w == 1 && ht == 1 {
                break;
            }
            w = (w >> 1).max(1);
            ht = (ht >> 1).max(1);
        }
        println!(
            "\n  {levels} level{}{}",
            if levels == 1 { "" } else { "s" },
            if pos == bytes.len() { "" } else { " (the size does not match a mip chain)" }
        );
    }
    u8::from(bad)
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() < 2 {
        usage(None);
    }
    let cmd = argv[1].as_str();
    if matches!(cmd, "-h" | "--help" | "help") {
        usage(None);
    }
    let o = parse_args(&argv[2..]);
    let code = match cmd {
        "generate" => cmd_generate(&o),
        "texture" => cmd_texture(&o),
        "verify" => cmd_verify(&o),
        "info" => cmd_info(&o),
        _ => usage(Some(&format!("unknown command {cmd}"))),
    };
    let _ = std::io::stdout().flush();
    ExitCode::from(code)
}
