# LeRobot Compatible Usage

## Uploading the dataset to HuggingFace

You need to setup the HuggingFace CLI and authenticate first. Follow the instructions [here](https://huggingface.co/docs/lerobot/il_robots#record-a-dataset).

Uploading the dataset allows you to visualize it in the online dataset viewer and also use it for training models using the HuggingFace tools.

As the LeRobot dataset does not expect you to upload large image files, you can exclude them using the `--exclude` flag. This does not delete the files locally, it just prevents them from being uploaded to HuggingFace. This will not affect the usability of the dataset for training models as the LeRobot models use video files instead of individual images.

```bash
 huggingface-cli upload TrossenRoboticsCommunity/record-test ~/.cache/huggingface/lerobot/TrossenRoboticsCommunity/test_dataset_03 --repo-type dataset --revision v2.1 --exclude *.jpg
```

Note:
  * The `--exclude *.jpg` flag is used to avoid uploading large image files.
  * The revision tag is important for passing the version checks in the online dataset viewer.


In case you want to upload all files including images, you can omit the `--exclude` flag.

```bash
 huggingface-cli upload TrossenRoboticsCommunity/record-test ~/.cache/huggingface/lerobot/TrossenRoboticsCommunity/test_dataset_03 --repo-type dataset --revision v2.1
```

The above command uploads all files including images.


## Replaying using LeRobot

If you have the `LeRobot` framework installed, you can also replay using the replay scripts.
Note: This was tested using the new api integration for Trossen Arms with LeRobot.

```bash
python -m lerobot.replay
--robot.type=bi_widowxai_follower
--robot.left_arm_ip_address=192.168.1.5
--robot.right_arm_ip_address=192.168.1.4
--robot.id=bimanual_follower
--dataset.repo_id=TrossenRoboticsCommunity/test_dataset_00
--dataset.episode=0
```

## Visualizing the dataset

You can visualize the recorded dataset using the `lerobot` dataset viewer. This requires you to have the `lerobot` package installed.

```bash
 python src/lerobot/scripts/visualize_dataset_html.py  --repo-id TrossenRoboticsCommunity/test_dataset_03
```

To view the dataset online in the HuggingFace dataset viewer, you can use the following link:

[LeRobot Dataset Viewer](https://huggingface.co/spaces/lerobot/visualize_dataset)

Just dataset name and repo id in the input box and click on "Go".


## Train a model using LeRobot

For this example you will need to have `lerobot` installed with the `smolvla` extra dependencies.

### SmolVLA


Install the `smolvla` extra dependencies for `lerobot`:

```bash
pip install -e ".[smolvla]"
```

Run the training command:

```bash
cd lerobot && lerobot-train \
  --policy.path=lerobot/smolvla_base \
  --dataset.repo_id=TrossenRoboticsCommunity/test_dataset_00 \
  --batch_size=4\
  --steps=20000 \
  --output_dir=outputs/train/smolvla_trossen_ai_stationary_test_training \
  --job_name=smolvla_training_trossen_ai_stationary_test_training \
  --policy.device=cuda \
  --wandb.enable=true \
  --policy.repo_id TrossenRoboticsCommunity/smolvla_trossen_ai_stationary_test_training
```

### ACT

```bash
lerobot-train \
  --dataset.repo_id=TrossenRoboticsCommunity/test_dataset_00 \
  --policy.type=act \
  --output_dir=outputs/train/act_trossen_ai_stationary_test_training \
  --job_name=act_trossen_ai_stationary_test_training \
  --policy.device=cuda \
  --wandb.enable=true \
  --policy.repo_id=TrossenRoboticsCommunity/act_trossen_ai_stationary_test_training \
  --steps=20000 \
  --batch_size=4
```
