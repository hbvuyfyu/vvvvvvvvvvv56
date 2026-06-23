package com.vcam.service

  import android.app.Notification
  import android.app.NotificationChannel
  import android.app.NotificationManager
  import android.app.PendingIntent
  import android.app.Service
  import android.content.Intent
  import android.net.Uri
  import android.os.Build
  import android.os.IBinder
  import androidx.core.app.NotificationCompat
  import com.vcam.R
  import com.vcam.ui.MainActivity
  import com.vcam.utils.CameraInjector
  import com.vcam.utils.MediaUtils
  import com.vcam.utils.RootManager
  import kotlinx.coroutines.CoroutineScope
  import kotlinx.coroutines.Dispatchers
  import kotlinx.coroutines.Job
  import kotlinx.coroutines.launch

  class VCamService : Service() {

      companion object {
          const val ACTION_START          = "com.vcam.ACTION_START"
          const val ACTION_STOP           = "com.vcam.ACTION_STOP"
          const val EXTRA_MEDIA_URI       = "extra_media_uri"
          const val EXTRA_TARGET_PACKAGE  = "extra_target_package"
          const val EXTRA_TARGET_NAME     = "extra_target_name"
          const val EXTRA_IS_VIDEO        = "extra_is_video"

          private const val CHANNEL_ID       = "vcam_channel"
          private const val NOTIFICATION_ID  = 1001
      }

      private val serviceScope = CoroutineScope(Dispatchers.IO + Job())
      private var injector: CameraInjector? = null

      override fun onBind(intent: Intent?): IBinder? = null

      override fun onCreate() {
          super.onCreate()
          createNotificationChannel()
      }

      override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
          when (intent?.action) {
              ACTION_START -> {
                  val mediaUriStr    = intent.getStringExtra(EXTRA_MEDIA_URI)    ?: return START_NOT_STICKY
                  val mediaUri       = Uri.parse(mediaUriStr)
                  val targetPackage  = intent.getStringExtra(EXTRA_TARGET_PACKAGE)
                  val targetName     = intent.getStringExtra(EXTRA_TARGET_NAME)  ?: targetPackage ?: "All Apps"
                  val isVideo        = intent.getBooleanExtra(EXTRA_IS_VIDEO, false)

                  // Show "Injection Active — open target app" notification
                  val notification = buildNotification(targetName, isVideo)
                  startForeground(NOTIFICATION_ID, notification)

                  serviceScope.launch { startInjection(mediaUri, targetPackage, targetName, isVideo) }
              }
              ACTION_STOP -> {
                  stopInjection()
                  stopForeground(STOP_FOREGROUND_REMOVE)
                  stopSelf()
              }
          }
          return START_STICKY
      }

      private suspend fun startInjection(
          mediaUri: Uri, targetPackage: String?,
          targetName: String, isVideo: Boolean
      ) {
          try {
              val mediaFile = MediaUtils.copyUriToFile(
                  this, mediaUri,
                  if (isVideo) "vcam_input.mp4" else "vcam_input.jpg"
              ) ?: return

              injector = CameraInjector(this, mediaFile.absolutePath, isVideo, targetPackage)
              injector?.start()

              // After injection is set up, update notification to tell user to open the app
              updateNotification(
                  "VCam Active",
                  if (!targetPackage.isNullOrBlank())
                      "Injection ready — open $targetName now"
                  else
                      "VCam active — open any camera app"
              )
          } catch (e: Exception) {
              e.printStackTrace()
          }
      }

      private fun stopInjection() {
          injector?.stop()
          injector = null
      }

      // ── Notifications ─────────────────────────────────────────────────────

      private fun createNotificationChannel() {
          if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
              val channel = NotificationChannel(
                  CHANNEL_ID,
                  "VCam Camera Injection",
                  NotificationManager.IMPORTANCE_LOW
              ).apply { description = "VCam injection service" }
              getSystemService(NotificationManager::class.java)?.createNotificationChannel(channel)
          }
      }

      private fun buildNotification(targetName: String, isVideo: Boolean): Notification {
          val pi = PendingIntent.getActivity(
              this, 0,
              Intent(this, MainActivity::class.java),
              PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
          )
          val stopPi = PendingIntent.getService(
              this, 1,
              Intent(this, VCamService::class.java).apply { action = ACTION_STOP },
              PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
          )
          val mediaType = if (isVideo) "video" else "image"
          return NotificationCompat.Builder(this, CHANNEL_ID)
              .setSmallIcon(android.R.drawable.ic_media_play)
              .setContentTitle("VCam — Injecting $mediaType")
              .setContentText("Setting up hook for $targetName…")
              .setContentIntent(pi)
              .addAction(android.R.drawable.ic_media_pause, "Stop VCam", stopPi)
              .setOngoing(true)
              .build()
      }

      private fun updateNotification(title: String, text: String) {
          val nm = getSystemService(NotificationManager::class.java) ?: return
          val pi = PendingIntent.getActivity(
              this, 0,
              Intent(this, MainActivity::class.java),
              PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
          )
          val stopPi = PendingIntent.getService(
              this, 1,
              Intent(this, VCamService::class.java).apply { action = ACTION_STOP },
              PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
          )
          val n = NotificationCompat.Builder(this, CHANNEL_ID)
              .setSmallIcon(android.R.drawable.ic_media_play)
              .setContentTitle(title)
              .setContentText(text)
              .setContentIntent(pi)
              .addAction(android.R.drawable.ic_media_pause, "Stop VCam", stopPi)
              .setOngoing(true)
              .build()
          nm.notify(NOTIFICATION_ID, n)
      }
  }
  