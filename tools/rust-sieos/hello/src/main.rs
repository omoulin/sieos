use std::sync::{Arc, Mutex, mpsc};
use std::{env, fs, io::Write, process, thread, time};

fn main() {
    println!("rust: hello from SIEOS, args {:?}", env::args().collect::<Vec<_>>());
    let n = Arc::new(Mutex::new(0u64));
    let (tx, rx) = mpsc::channel();
    let hs: Vec<_> = (0..8).map(|i| {
        let (n, tx) = (n.clone(), tx.clone());
        thread::spawn(move || {
            for _ in 0..10000 { *n.lock().unwrap() += 1; }
            tx.send(i).unwrap();
        })
    }).collect();
    for h in hs { h.join().unwrap(); }
    drop(tx);
    println!("rust: threads {} sends {}", *n.lock().unwrap(), rx.iter().count());
    let p = env::temp_dir().join("rust-test.txt");
    fs::File::create(&p).unwrap().write_all(b"sieos").unwrap();
    let m = fs::metadata(&p).unwrap();
    println!("rust: file len {} mode {:o} modified {:?}", m.len(), std::os::unix::fs::PermissionsExt::mode(&m.permissions()), m.modified().is_ok());
    println!("rust: read {:?}", fs::read_to_string(&p).unwrap());
    fs::remove_file(&p).unwrap();
    println!("rust: missing file error: {}", fs::read("/nonexistent").unwrap_err());
    let out = process::Command::new("/bin/echo").arg("child ok").output().unwrap();
    println!("rust: child {:?} status {}", String::from_utf8_lossy(&out.stdout).trim(), out.status);
    let t0 = time::Instant::now();
    thread::sleep(time::Duration::from_millis(50));
    println!("rust: slept {} ms; now {:?}", t0.elapsed().as_millis(), time::SystemTime::now().duration_since(time::UNIX_EPOCH).unwrap().as_secs() > 0);
    let entries = fs::read_dir("/").unwrap().count();
    println!("rust: / has {} entries; cwd {:?}", entries, env::current_dir().unwrap());
    unsafe {
        let e = || std::io::Error::last_os_error();
        let fd = libc::socket(libc::AF_INET, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0);
        println!("rust: socket {} {} (AF_INET {} SOCK_STREAM {} SOCK_CLOEXEC {:#x})", fd, e(), libc::AF_INET, libc::SOCK_STREAM, libc::SOCK_CLOEXEC);
        let one: libc::c_int = 1;
        let r = libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, &one as *const _ as *const libc::c_void, 4);
        println!("rust: setsockopt {} {} (SOL_SOCKET {:#x} SO_REUSEADDR {:#x})", r, e(), libc::SOL_SOCKET, libc::SO_REUSEADDR);
        let mut a: libc::sockaddr_in = std::mem::zeroed();
        a.sin_family = libc::AF_INET as _; a.sin_addr.s_addr = u32::from_be_bytes([127, 0, 0, 1]).to_be();
        let r = libc::bind(fd, &a as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_in>() as _);
        println!("rust: bind {} {} (sockaddr_in {} bytes)", r, e(), std::mem::size_of::<libc::sockaddr_in>());
    }
    let l = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let a = l.local_addr().unwrap();
    let c = thread::spawn(move || { let mut s = std::net::TcpStream::connect(a).unwrap(); s.write_all(b"tcp").unwrap(); });
    let (mut s, _) = l.accept().unwrap();
    let mut b = String::new();
    std::io::Read::read_to_string(&mut s, &mut b).unwrap();
    c.join().unwrap();
    println!("rust: tcp {:?} via {}", b, a);
    println!("rust: done");
}
