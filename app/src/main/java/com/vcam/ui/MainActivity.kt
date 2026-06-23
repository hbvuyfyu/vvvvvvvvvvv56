package com.vcam.ui

  import android.Manifest
  import android.content.Intent
  import android.content.pm.PackageManager
  import android.net.Uri
  import android.os.Build
  import android.os.Bundle
  import android.view.View
  import androidx.activity.result.contract.ActivityResultContracts
  import androidx.activity.viewModels
  import androidx.appcompat.app.AppCompatActivity
  import androidx.core.content.ContextCompat
  import androidx.lifecycle.lifecycleScope
  import androidx.recyclerview.widget.LinearLayoutManager
  import com.google.android.material.dialog.MaterialAlertDialogBuilder
  import com.google.android.material.snackbar.Snackbar
  import com.vcam.R
  import com.vcam.databinding.ActivityMainBinding
  import com.vcam.model.AppInfo
  import com.vcam.service.VCamService
  import com.vcam.ui.adapter.AppListAdapter
  import com.vcam.viewmodel.MainViewModel
  import kotlinx.coroutines.launch

  class MainActivity : AppCompatActivity() {

      private lateinit var binding: ActivityMainBinding
      private val viewModel: MainViewModel by viewModels()
      private lateinit var appListAdapter: AppListAdapter

      private val pickMedia = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
          uri?.let { viewModel.setMediaUri(it, this) }
      }

      private val permissionLauncher = registerForActivityResult(
          ActivityResultContracts.RequestMultiplePermissions()
      ) { permissions ->
          val allGranted = permissions.values.all { it }
          // FIX: initRoot() must be called here too, not just when permissions were pre-granted
          lifecycleScope.launch {
              viewModel.initRoot()
              if (allGranted) {
                  viewModel.loadInstalledApps(this@MainActivity)
              } else {
                  showSnack(getString(R.string.permissions_required))
              }
          }
      }

      override fun onCreate(savedInstanceState: Bundle?) {
          super.onCreate(savedInstanceState)
          binding = ActivityMainBinding.inflate(layoutInflater)
          setContentView(binding.root)
          setupToolbar()
          setupRecyclerView()
          setupObservers()
          setupClickListeners()
          requestPermissions()
      }

      private fun setupToolbar() { setSupportActionBar(binding.toolbar) }

      private fun setupRecyclerView() {
          appListAdapter = AppListAdapter { app -> viewModel.selectTargetApp(app) }
          binding.rvApps.apply {
              layoutManager = LinearLayoutManager(this@MainActivity)
              adapter = appListAdapter
          }
      }

      private fun setupObservers() {
          viewModel.filteredApps.observe(this) { apps ->
              appListAdapter.submitList(apps)
              binding.progressApps.visibility = View.GONE
              binding.tvNoApps.visibility = if (apps.isEmpty()) View.VISIBLE else View.GONE
          }

          viewModel.selectedApp.observe(this) { app ->
              if (app != null) {
                  binding.tvSelectedApp.text = app.appName
                  binding.cardSelectedApp.visibility = View.VISIBLE
              } else {
                  binding.cardSelectedApp.visibility = View.GONE
              }
          }

          viewModel.mediaUri.observe(this) { uri ->
              if (uri != null) {
                  binding.tvMediaSelected.text = getString(R.string.media_selected)
                  binding.ivMediaPreview.setImageURI(uri)
                  binding.cardMedia.visibility = View.VISIBLE
                  binding.layoutMediaType.visibility = View.VISIBLE
                  updateStartButton()
              } else {
                  binding.cardMedia.visibility = View.GONE
                  binding.layoutMediaType.visibility = View.GONE
              }
          }

          viewModel.isServiceRunning.observe(this) { running ->
              binding.btnStartStop.text = if (running) getString(R.string.stop_vcam) else getString(R.string.start_vcam)
              binding.btnStartStop.setIconResource(if (running) R.drawable.ic_stop else R.drawable.ic_play)
              val color = ContextCompat.getColor(this, if (running) R.color.color_stop else R.color.color_start)
              binding.btnStartStop.setBackgroundColor(color)
              binding.statusIndicator.visibility = if (running) View.VISIBLE else View.GONE
          }

          viewModel.rootStatus.observe(this) { hasRoot ->
              binding.tvRootStatus.text = if (hasRoot) getString(R.string.root_granted) else getString(R.string.root_denied)
              val color = ContextCompat.getColor(this, if (hasRoot) R.color.color_root_ok else R.color.color_root_fail)
              binding.tvRootStatus.setTextColor(color)
              binding.ivRootIcon.setColorFilter(color)
          }

          viewModel.errorMessage.observe(this) { msg ->
              if (msg != null) { showSnack(msg); viewModel.clearError() }
          }

          viewModel.isLoading.observe(this) { loading ->
              binding.progressApps.visibility = if (loading) View.VISIBLE else View.GONE
          }
      }

      private fun setupClickListeners() {
          binding.btnPickImage.setOnClickListener { pickMedia.launch("image/*") }
          binding.btnPickVideo.setOnClickListener { pickMedia.launch("video/*") }

          binding.btnStartStop.setOnClickListener {
              if (viewModel.isServiceRunning.value == true) stopVCamService() else startVCamService()
          }
          binding.btnClearMedia.setOnClickListener { viewModel.clearMedia() }
          binding.btnClearApp.setOnClickListener { viewModel.clearSelectedApp() }
          binding.btnRefreshApps.setOnClickListener {
              binding.progressApps.visibility = View.VISIBLE
              viewModel.loadInstalledApps(this)
          }

          binding.etSearchApps.addTextChangedListener(object : android.text.TextWatcher {
              override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
              override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                  viewModel.filterApps(s?.toString() ?: "")
              }
              override fun afterTextChanged(s: android.text.Editable?) {}
          })

          binding.chipGroup.setOnCheckedStateChangeListener { _, checkedIds ->
              viewModel.setAppFilter(checkedIds.firstOrNull())
          }
      }

      private fun startVCamService() {
          val mediaUri = viewModel.mediaUri.value ?: run {
              showSnack(getString(R.string.select_media_first)); return
          }
          if (viewModel.rootStatus.value != true) {
              showSnack("Root not granted — please allow root in your root manager and restart the app")
              return
          }
          val targetApp = viewModel.selectedApp.value
          if (targetApp == null) {
              MaterialAlertDialogBuilder(this)
                  .setTitle(R.string.no_target_app)
                  .setMessage(R.string.no_target_app_message)
                  .setPositiveButton(R.string.start_anyway) { _, _ -> doStartService(mediaUri, null) }
                  .setNegativeButton(android.R.string.cancel, null)
                  .show()
          } else {
              doStartService(mediaUri, targetApp)
          }
      }

      private fun doStartService(mediaUri: Uri, targetApp: AppInfo?) {
          val intent = Intent(this, VCamService::class.java).apply {
              action = VCamService.ACTION_START
              putExtra(VCamService.EXTRA_MEDIA_URI, mediaUri.toString())
              targetApp?.let {
                  putExtra(VCamService.EXTRA_TARGET_PACKAGE, it.packageName)
                  putExtra(VCamService.EXTRA_TARGET_NAME, it.appName)
              }
              putExtra(VCamService.EXTRA_IS_VIDEO, viewModel.isVideo.value == true)
          }
          if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(intent)
          else startService(intent)
          viewModel.setServiceRunning(true)
      }

      private fun stopVCamService() {
          startService(Intent(this, VCamService::class.java).apply { action = VCamService.ACTION_STOP })
          viewModel.setServiceRunning(false)
      }

      private fun updateStartButton() {
          binding.btnStartStop.isEnabled = viewModel.mediaUri.value != null
      }

      private fun requestPermissions() {
          val permissions = buildList {
              if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                  add(Manifest.permission.READ_MEDIA_IMAGES)
                  add(Manifest.permission.READ_MEDIA_VIDEO)
              } else {
                  add(Manifest.permission.READ_EXTERNAL_STORAGE)
              }
              add(Manifest.permission.CAMERA)
          }
          val toRequest = permissions.filter {
              ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
          }
          if (toRequest.isNotEmpty()) {
              permissionLauncher.launch(toRequest.toTypedArray())
          } else {
              lifecycleScope.launch {
                  viewModel.initRoot()
                  viewModel.loadInstalledApps(this@MainActivity)
              }
          }
      }

      private fun showSnack(msg: String) = Snackbar.make(binding.root, msg, Snackbar.LENGTH_LONG).show()
  }
  