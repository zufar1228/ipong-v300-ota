package com.ipong.v300;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.Bitmap;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.Uri;
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

/**
 * Thin WebView wrapper around the robot's control page (http://192.168.4.1).
 *
 * The robot's access point has no internet, so Android may route traffic over
 * mobile data or drop the WiFi. This activity requests the WiFi network
 * explicitly (without requiring internet) and binds the whole process to it,
 * so the page and its WebSocket always go over the robot's WiFi.
 */
public class MainActivity extends Activity {
    private static final String ROBOT_URL = "http://192.168.4.1/";
    private static final String ROBOT_HOST = "192.168.4.1";
    private static final long RETRY_MS = 4000;

    private WebView web;
    private ConnectivityManager cm;
    private ConnectivityManager.NetworkCallback wifiCallback;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private boolean showingError = false;

    private final Runnable retry = new Runnable() {
        @Override
        public void run() {
            if (showingError) loadRobot();
        }
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
        s.setMediaPlaybackRequiresUserGesture(true);
        s.setUserAgentString(s.getUserAgentString() + " iPongApp/1.0");

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri u = request.getUrl();
                String scheme = u.getScheme() == null ? "" : u.getScheme();
                if (scheme.equals("ipong")) {
                    if ("wifi".equals(u.getHost())) openWifiSettings();
                    else loadRobot();
                    return true;
                }
                if (ROBOT_HOST.equals(u.getHost())) return false;
                // Any other link (e.g. GitHub) opens in the regular browser.
                try {
                    startActivity(new Intent(Intent.ACTION_VIEW, u));
                } catch (ActivityNotFoundException ignored) {
                }
                return true;
            }

            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                if (url != null && url.startsWith(ROBOT_URL)) showingError = false;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) showError();
            }
        });

        cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
        requestWifi();
        loadRobot();
    }

    /** Ask for any WiFi network, even one without internet, and route this app through it. */
    private void requestWifi() {
        if (cm == null) return;
        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .build();
        wifiCallback = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network network) {
                cm.bindProcessToNetwork(network);
                handler.post(() -> {
                    if (showingError) loadRobot();
                });
            }

            @Override
            public void onLost(Network network) {
                cm.bindProcessToNetwork(null);
            }
        };
        try {
            cm.requestNetwork(req, wifiCallback);
        } catch (SecurityException | IllegalArgumentException e) {
            wifiCallback = null;
        }
    }

    private void loadRobot() {
        handler.removeCallbacks(retry);
        web.loadUrl(ROBOT_URL);
    }

    private void openWifiSettings() {
        try {
            startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
        } catch (ActivityNotFoundException ignored) {
        }
    }

    private void showError() {
        showingError = true;
        web.loadDataWithBaseURL(null, ERROR_HTML, "text/html", "utf-8", null);
        handler.removeCallbacks(retry);
        handler.postDelayed(retry, RETRY_MS);
    }

    @Override
    protected void onResume() {
        super.onResume();
        web.onResume();
        if (showingError) loadRobot();
    }

    @Override
    protected void onPause() {
        web.onPause();
        super.onPause();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (!showingError && web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }

    @Override
    protected void onDestroy() {
        handler.removeCallbacksAndMessages(null);
        if (cm != null && wifiCallback != null) {
            try {
                cm.unregisterNetworkCallback(wifiCallback);
            } catch (IllegalArgumentException ignored) {
            }
            cm.bindProcessToNetwork(null);
        }
        web.destroy();
        super.onDestroy();
    }

    private static final String ERROR_HTML = "<!DOCTYPE html><html lang='id'><head><meta charset='utf-8'>"
            + "<meta name='viewport' content='width=device-width,initial-scale=1'><style>"
            + "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
            + "background:linear-gradient(180deg,#12407a,#0a1a33 70%);color:#f4f7fb;font-family:system-ui,sans-serif}"
            + ".w{max-width:420px;padding:24px 18px;text-align:center}"
            + ".b{width:56px;height:56px;border-radius:50%;margin:0 auto 16px;"
            + "background:radial-gradient(circle at 35% 30%,#fff,#ffb35c 25%,#ff8a1f 70%);animation:j 1.2s infinite}"
            + "@keyframes j{50%{transform:translateY(-14px)}}"
            + "h1{font-size:19px;margin:0 0 8px}p{color:#a9bdd8;font-size:14px;line-height:1.6;margin:0 0 18px}"
            + ".n{text-align:left;background:rgba(245,158,11,.12);border:1px solid rgba(245,158,11,.5);border-radius:14px;"
            + "padding:12px 14px;font-size:13px;line-height:1.6;color:#fde7c0;margin-bottom:18px}"
            + "a{display:block;padding:14px;border-radius:14px;text-decoration:none;font-weight:700;margin-bottom:10px}"
            + ".p{background:linear-gradient(135deg,#ffb35c,#ff8a1f);color:#2a1200}"
            + ".s{background:rgba(255,255,255,.08);color:#fff;border:1px solid rgba(255,255,255,.12)}"
            + "</style></head><body><div class='w'><div class='b'></div>"
            + "<h1>Robot tidak dapat dijangkau</h1>"
            + "<p>Aplikasi akan mencoba menyambung kembali secara otomatis.</p>"
            + "<div class='n'>Mohon pastikan perangkat Anda terhubung ke jaringan <b>natsir'S-ipongv300</b> "
            + "dan robot dalam keadaan menyala. Apabila perangkat beralih secara otomatis ke jaringan WiFi lain, "
            + "silakan pilih kembali jaringan robot melalui Pengaturan WiFi.</div>"
            + "<a class='p' href='ipong://retry'>Coba Lagi</a>"
            + "<a class='s' href='ipong://wifi'>Buka Pengaturan WiFi</a>"
            + "</div></body></html>";
}
