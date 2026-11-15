package com.ipong.v300;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.WindowManager;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.net.HttpURLConnection;
import java.net.URL;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * WebView wrapper around the robot's control page.
 *
 * Connection order:
 *  1. Robot AP (192.168.4.1) already reachable over the current WiFi.
 *  2. Robot found on the home WiFi through mDNS/NSD (_ipong._tcp, "Mode WiFi Rumah").
 *  3. Android 10+: ask the system to join the robot AP with WifiNetworkSpecifier.
 *     While the app holds this request Android will not switch to another WiFi;
 *     when the app closes, the phone returns to its usual (home) WiFi.
 * The process is bound to the chosen network so traffic never leaks to mobile data.
 */
public class MainActivity extends Activity {
    private static final String AP_SSID = "natsir'S-ipongv300";
    private static final String AP_PASS = "44444444";
    private static final String AP_IP = "192.168.4.1";
    private static final String NSD_TYPE = "_ipong._tcp.";
    private static final int PROBE_TIMEOUT_MS = 1500;
    private static final long NSD_TIMEOUT_MS = 4000;
    private static final int JOIN_TIMEOUT_MS = 45000;
    private static final long RELOAD_MS = 4000;

    private WebView web;
    private ConnectivityManager cm;
    private NsdManager nsd;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newSingleThreadExecutor();

    private ConnectivityManager.NetworkCallback netCallback;
    private NsdManager.DiscoveryListener nsdListener;
    private String robotUrl = null;     // page currently in use
    private int attempt = 0;            // invalidates callbacks of older attempts
    private boolean showingError = false;

    private final Runnable reload = () -> {
        if (showingError && robotUrl != null) web.loadUrl(robotUrl);
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        web = new WebView(this);
        web.setBackgroundColor(0xFF0A1A33);
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setUserAgentString(s.getUserAgentString() + " iPongApp/1.1");

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri u = request.getUrl();
                if ("ipong".equals(u.getScheme())) {
                    if ("wifi".equals(u.getHost())) openWifiSettings();
                    else connect();
                    return true;
                }
                if (robotUrl != null && u.toString().startsWith(robotUrl)) return false;
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, u));
                } catch (ActivityNotFoundException ignored) {
                }
                return true;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) showError(null);
            }
        });

        cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
        nsd = (NsdManager) getSystemService(Context.NSD_SERVICE);
        connect();
    }

    // ---------------------------------------------------------------- connection flow

    private void connect() {
        final int id = ++attempt;
        cleanupNetwork();
        robotUrl = null;
        showingError = false;
        ui.removeCallbacks(reload);
        showStatus("Mencari robot...");

        io.execute(() -> {
            // 1. Already on the robot AP?
            Network wifi = findWifiNetwork();
            if (wifi != null && probe(wifi, AP_IP)) {
                ui.post(() -> { if (id == attempt) open(wifi, AP_IP); });
                return;
            }
            // 2. Robot on the home WiFi (mDNS).
            ui.post(() -> { if (id == attempt) discoverOnLan(id, wifi); });
        });
    }

    private void discoverOnLan(int id, Network wifi) {
        if (nsd == null || wifi == null) {
            joinRobotAp(id);
            return;
        }
        showStatus("Mencari robot di jaringan WiFi ini...");
        final boolean[] done = {false};
        nsdListener = new NsdManager.DiscoveryListener() {
            @Override public void onDiscoveryStarted(String t) { }
            @Override public void onDiscoveryStopped(String t) { }
            @Override public void onStartDiscoveryFailed(String t, int e) { }
            @Override public void onStopDiscoveryFailed(String t, int e) { }
            @Override public void onServiceLost(NsdServiceInfo i) { }

            @Override
            public void onServiceFound(NsdServiceInfo info) {
                if (done[0]) return;
                done[0] = true;
                resolve(info, id, wifi);
            }
        };
        try {
            nsd.discoverServices(NSD_TYPE, NsdManager.PROTOCOL_DNS_SD, nsdListener);
        } catch (IllegalArgumentException e) {
            nsdListener = null;
        }
        ui.postDelayed(() -> {
            if (id != attempt || done[0]) return;
            done[0] = true;
            stopDiscovery();
            joinRobotAp(id);
        }, NSD_TIMEOUT_MS);
    }

    @SuppressWarnings("deprecation")
    private void resolve(NsdServiceInfo info, int id, Network wifi) {
        nsd.resolveService(info, new NsdManager.ResolveListener() {
            @Override
            public void onResolveFailed(NsdServiceInfo i, int e) {
                ui.post(() -> { if (id == attempt) { stopDiscovery(); joinRobotAp(id); } });
            }

            @Override
            public void onServiceResolved(NsdServiceInfo i) {
                String host = i.getHost() == null ? null : i.getHost().getHostAddress();
                io.execute(() -> {
                    boolean ok = host != null && probe(wifi, host);
                    ui.post(() -> {
                        if (id != attempt) return;
                        stopDiscovery();
                        if (ok) open(wifi, host);
                        else joinRobotAp(id);
                    });
                });
            }
        });
    }

    /** 3. Join the robot AP. Android 10+ shows a one-time "connect to device" dialog. */
    private void joinRobotAp(int id) {
        if (id != attempt) return;
        if (cm == null) {
            showError("Layanan jaringan tidak tersedia.");
            return;
        }
        NetworkRequest.Builder b = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            b.setNetworkSpecifier(new WifiNetworkSpecifier.Builder()
                    .setSsid(AP_SSID)
                    .setWpa2Passphrase(AP_PASS)
                    .build());
            showStatus("Menghubungkan ke WiFi robot...<br><small>Setujui permintaan sambungan yang muncul di layar.</small>");
        } else {
            showStatus("Menunggu WiFi robot...");
        }
        netCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                io.execute(() -> {
                    // DHCP may need a moment after the link comes up.
                    boolean ok = false;
                    for (int i = 0; i < 5 && !ok; i++) {
                        ok = probe(network, AP_IP);
                        if (!ok) sleep(800);
                    }
                    final boolean reachable = ok;
                    ui.post(() -> {
                        if (id != attempt) return;
                        if (reachable) open(network, AP_IP);
                        else showError(null);
                    });
                });
            }

            @Override
            public void onUnavailable() {
                ui.post(() -> {
                    if (id == attempt) showError("WiFi robot tidak ditemukan atau permintaan sambungan dibatalkan.");
                });
            }

            @Override
            public void onLost(Network network) {
                ui.post(() -> {
                    if (id == attempt) showError(null);
                });
            }
        };
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) cm.requestNetwork(b.build(), netCallback, JOIN_TIMEOUT_MS);
            else cm.requestNetwork(b.build(), netCallback);
        } catch (RuntimeException e) {
            netCallback = null;
            showError("Permintaan sambungan WiFi ditolak oleh sistem.");
        }
    }

    private void open(Network network, String host) {
        if (cm != null && network != null) cm.bindProcessToNetwork(network);
        robotUrl = "http://" + host + "/";
        showingError = false;
        web.loadUrl(robotUrl);
    }

    // ---------------------------------------------------------------- helpers

    @SuppressWarnings("deprecation")
    private Network findWifiNetwork() {
        if (cm == null) return null;
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities c = cm.getNetworkCapabilities(n);
            if (c != null && c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return n;
        }
        return null;
    }

    /** Checks that an iPong robot answers at host, using the given network. */
    private static boolean probe(Network network, String host) {
        HttpURLConnection c = null;
        try {
            URL url = new URL("http://" + host + "/manifest.json");
            c = (HttpURLConnection) network.openConnection(url);
            c.setConnectTimeout(PROBE_TIMEOUT_MS);
            c.setReadTimeout(PROBE_TIMEOUT_MS);
            c.setUseCaches(false);
            if (c.getResponseCode() != 200) return false;
            byte[] buf = new byte[256];
            int n = c.getInputStream().read(buf);
            return n > 0 && new String(buf, 0, n, "UTF-8").contains("iPong");
        } catch (Exception e) {
            return false;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    private void stopDiscovery() {
        if (nsd != null && nsdListener != null) {
            try {
                nsd.stopServiceDiscovery(nsdListener);
            } catch (IllegalArgumentException ignored) {
            }
        }
        nsdListener = null;
    }

    private void cleanupNetwork() {
        stopDiscovery();
        if (cm != null && netCallback != null) {
            try {
                cm.unregisterNetworkCallback(netCallback);
            } catch (IllegalArgumentException ignored) {
            }
        }
        netCallback = null;
        if (cm != null) cm.bindProcessToNetwork(null);
    }

    private void openWifiSettings() {
        try {
            startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
        } catch (ActivityNotFoundException ignored) {
        }
    }

    // ---------------------------------------------------------------- screens

    private void showStatus(String html) {
        web.loadDataWithBaseURL(null, page("<div class='b'></div><h1>iPong V300</h1><p>" + html + "</p>"),
                "text/html", "utf-8", null);
    }

    private void showError(String reason) {
        showingError = true;
        String why = reason != null ? reason : "Robot tidak dapat dijangkau.";
        web.loadDataWithBaseURL(null, page("<div class='b'></div><h1>" + why + "</h1>"
                + "<div class='n'>Mohon pastikan robot dalam keadaan menyala. Aplikasi akan menghubungkan perangkat "
                + "ke jaringan <b>natsir'S-ipongv300</b> secara otomatis, atau ke robot di jaringan WiFi rumah apabila "
                + "Mode WiFi Rumah telah diaktifkan.</div>"
                + "<a class='p' href='ipong://retry'>Coba Lagi</a>"
                + "<a class='s' href='ipong://wifi'>Buka Pengaturan WiFi</a>"), "text/html", "utf-8", null);
        ui.removeCallbacks(reload);
        if (robotUrl != null) ui.postDelayed(reload, RELOAD_MS);
    }

    private static String page(String body) {
        return "<!DOCTYPE html><html lang='id'><head><meta charset='utf-8'>"
                + "<meta name='viewport' content='width=device-width,initial-scale=1'><style>"
                + "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
                + "background:linear-gradient(180deg,#12407a,#0a1a33 70%);color:#f4f7fb;font-family:system-ui,sans-serif}"
                + ".w{max-width:420px;padding:24px 18px;text-align:center}"
                + ".b{width:56px;height:56px;border-radius:50%;margin:0 auto 16px;"
                + "background:radial-gradient(circle at 35% 30%,#fff,#ffb35c 25%,#ff8a1f 70%);animation:j 1.2s infinite}"
                + "@keyframes j{50%{transform:translateY(-14px)}}"
                + "h1{font-size:19px;margin:0 0 10px}p{color:#a9bdd8;font-size:14px;line-height:1.6;margin:0 0 18px}"
                + "small{font-size:12px}"
                + ".n{text-align:left;background:rgba(245,158,11,.12);border:1px solid rgba(245,158,11,.5);border-radius:14px;"
                + "padding:12px 14px;font-size:13px;line-height:1.6;color:#fde7c0;margin-bottom:18px}"
                + "a{display:block;padding:14px;border-radius:14px;text-decoration:none;font-weight:700;margin-bottom:10px}"
                + ".p{background:linear-gradient(135deg,#ffb35c,#ff8a1f);color:#2a1200}"
                + ".s{background:rgba(255,255,255,.08);color:#fff;border:1px solid rgba(255,255,255,.12)}"
                + "</style></head><body><div class='w'>" + body + "</div></body></html>";
    }

    // ---------------------------------------------------------------- lifecycle

    @Override
    protected void onResume() {
        super.onResume();
        web.onResume();
    }

    @Override
    protected void onPause() {
        web.onPause();
        super.onPause();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (!showingError && robotUrl != null && web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        attempt++;
        ui.removeCallbacksAndMessages(null);
        cleanupNetwork();  // releases the robot WiFi: the phone returns to its usual network
        io.shutdownNow();
        web.destroy();
        super.onDestroy();
    }
}
