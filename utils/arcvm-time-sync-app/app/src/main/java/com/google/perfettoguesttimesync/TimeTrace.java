// Copyright 2021 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.package com.google.perfettoguesttimesync;
package com.google.perfettoguesttimesync;

import android.app.IntentService;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

public class TimeTrace extends IntentService {

    private static final String CHANNEL_ID = "TimeTrace";
    private static final int ONGOING_NOTIFICATION_ID = 12345;

    static {
        System.loadLibrary("perfettoguesttimesync");
    }

    public TimeTrace() {
        super("TimeTrace");
    }

    @Override
    protected void onHandleIntent(Intent workIntent) {
        Intent notificationIntent = new Intent(this, TimeTrace.class);
        PendingIntent pendingIntent =
                PendingIntent.getActivity(this, 0, notificationIntent, 0);

        String channelId = CHANNEL_ID;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID, "TimeTrace",
                    NotificationManager.IMPORTANCE_MIN);
            NotificationManager service =
                    (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            service.createNotificationChannel(channel);
        }

        Notification notification =
                new Notification.Builder(this, channelId)
                        .setContentTitle(getText(R.string.notification_title))
                        .setContentText(getText(R.string.notification_message))
                        .setContentIntent(pendingIntent)
                        .setTicker(getText(R.string.ticker_text))
                        .build();

        startForeground(ONGOING_NOTIFICATION_ID, notification);
        perfettoInit();
        while(true) {
            try {
                Thread.sleep(1000 * 1000);
            } catch (InterruptedException e) {
            }
        }
    }

    public native void perfettoInit();
}
