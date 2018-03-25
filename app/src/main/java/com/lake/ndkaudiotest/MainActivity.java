package com.lake.ndkaudiotest;

import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.os.Environment;
import android.support.v7.app.AppCompatActivity;
import android.os.Bundle;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.ListView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.File;

public class MainActivity extends AppCompatActivity implements View.OnClickListener {
    private SeekBar mSeekBar;
    private Thread timeThread;
    private int seekTimeP;
    private int mProgress;
    private ListView listview;
    private TextView tVTime;
    private TextView tVName;
    private TextView tTTime;
    private int toTalTime;
    private Button mBtnPlayOrPause, mBtnLast, mBtnNext;
    String inputurl;
    boolean isFirst = true;
    private int curItem = 0;
    boolean playing = false;
    private int length = 0;

    // Used to load the 'native-lib' library on application startup.
    static {
        System.loadLibrary("native-lib");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        initView();
        mSeekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                Log.e("lake", "onProgressChanged: " + progress);
                mProgress = progress;
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
                pause(true);
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                seek(mProgress);
            }
        });

    }

    private void initView() {
        listview = findViewById(R.id.listview);
        tVName = findViewById(R.id.filename);
        tVTime = findViewById(R.id.showtime);
        tTTime = findViewById(R.id.totaltime);
        mSeekBar = findViewById(R.id.seekbar);

        mBtnPlayOrPause = findViewById(R.id.playorpause);
        mBtnLast = findViewById(R.id.last);
        mBtnNext = findViewById(R.id.next);
        mBtnPlayOrPause.setOnClickListener(this);
        mBtnLast.setOnClickListener(this);
        mBtnNext.setOnClickListener(this);


        final String folderurl = Environment.getExternalStorageDirectory().getPath();
        final File[] files = new File(folderurl + "/MyLocalPlayer").listFiles();
        length = files.length;
        final ListFileAdapter myListAdapter = new ListFileAdapter(this, files);
        listview.setAdapter(myListAdapter);
        //myListAdapter.setSelectItem(0);
        listview.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                curItem = position;
                myListAdapter.setSelectItem(position);
                myListAdapter.notifyDataSetInvalidated();
                inputurl = folderurl + "/MyLocalPlayer/" + files[position].getName();
                if (!isFirst) {
                    stop();
                    mSeekBar.setProgress(0);
                }
                play(inputurl);
                if (isFirst) {
                    isFirst = false;
                    timeThread = new Thread(new Runnable() {
                        @Override
                        public void run() {
                            showtime();
                        }
                    });
                    timeThread.start();
                }
                pause(!playing);
            }
        });
        clickListItem(curItem);

    }

    @Override
    public void onClick(View v) {
        switch (v.getId()) {
            case R.id.playorpause:
                pause(playing);
                mBtnPlayOrPause.setText(playing ? "Play" : "Pause");
                playing = !playing;
                break;
            case R.id.last: {
                int item = (curItem - 1) < 0 ? length-1 : curItem - 1;
                clickListItem(item);
                break;
            }
            case R.id.next: {
                int item = (curItem + 1) >= length ? 0 : curItem + 1;
                clickListItem(item);
                break;
            }
            default:
                break;
        }
    }

    public void clickListItem(int position){
        AdapterView.OnItemClickListener onItemClickListener = listview.getOnItemClickListener();
        if (onItemClickListener != null) {
            onItemClickListener.onItemClick(listview, null, position, position);
            listview.setSelection(position);
        }
    }

    /**
     * 关闭播放器
     */
    public void shutdown() {
        stop();
        mSeekBar.setProgress(0);
        play(inputurl);
    }

    /**
     * 显示实时进度时间
     *
     * @param time
     */
    public void showTime(final int time) {
        final String n = resetTimeInt(time / 3600) + ":" + resetTimeInt(time % 3600 / 60) + ":" + resetTimeInt(time % 60);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                tVTime.setText(n);
                mSeekBar.setProgress(time);
            }
        });
        Log.e("lake", "showTime: " + n);
    }

    /**
     * 设置总时间
     *
     * @param total
     */
    public void setToatalTime(int total) {
        toTalTime = total;
        mSeekBar.setMax(total);
        Log.e("lake", "toTalTime: " + toTalTime + ",seekTimeP:" + seekTimeP);
        final String t = resetTimeInt(total / 3600) + ":" + resetTimeInt(total % 3600 / 60) + ":" + resetTimeInt(total % 60);
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                tTTime.setText(t);
            }
        });
    }

    /**
     * 播放结束
     *
     * @param isEnd
     */
    public void isPlayEnd(boolean isEnd) {
        Log.e("lake", "isPlayEnd: " + isEnd);
        if (isEnd) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    AdapterView.OnItemClickListener onItemClickListener = listview.getOnItemClickListener();
                    if (onItemClickListener != null) {
                        onItemClickListener.onItemClick(listview, null, curItem + 1, curItem + 1);
                    }
                    pause(false);
                }
            });
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
    }

    public native void play(String url);

    public native void stop();

    public native void pause(boolean play);

    public native void seek(int seekTime);

    public native void showtime();

    public String resetTimeInt(int time) {
        if (time < 10) {
            return "0" + time;
        } else {
            return time + "";
        }
    }

    class ListFileAdapter extends BaseAdapter {
        private Context context;
        private File[] files;

        public ListFileAdapter(Context context, File[] files) {
            this.context = context;
            this.files = files;
        }

        @Override
        public int getCount() {
            return files.length;
        }

        @Override
        public Object getItem(int position) {
            return files[position];
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            ViewHolder viewHolder = null;
            if (convertView == null) {
                viewHolder = new ViewHolder();
                convertView = LayoutInflater.from(context).inflate(R.layout.list_item, null);
                viewHolder.mTextView = (TextView) convertView.findViewById(R.id.filename);
                convertView.setTag(viewHolder);
            } else {
                viewHolder = (ViewHolder) convertView.getTag();
            }
            viewHolder.mTextView.setText(files[position].getName());
            if (position == selectItem) {
                convertView.setBackgroundColor(Color.GRAY);
            } else {
                convertView.setBackgroundColor(Color.WHITE);
            }
            return convertView;
        }

        class ViewHolder {
            TextView mTextView;
        }

        public void setSelectItem(int selectItem) {
            this.selectItem = selectItem;
        }

        private int selectItem = -1;
    }
}
