package app.squadspeak;

import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import org.qtproject.qt.android.bindings.QtActivity;

/** Keeps the Qt surface inside the usable area of edge-to-edge Android windows. */
public final class Activity extends QtActivity {
    @Override
    public void onCreate(final Bundle state) {
        super.onCreate(state);
        if (Build.VERSION.SDK_INT >= 35) {
            final View content = findViewById(android.R.id.content);
            content.setOnApplyWindowInsetsListener((view, insets) -> {
                final var covered = insets.getInsets(WindowInsets.Type.ime()
                    | WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                view.setPadding(covered.left, covered.top, covered.right, covered.bottom);
                return insets.inset(covered.left, covered.top, covered.right, covered.bottom);
            });
        }
    }
}
