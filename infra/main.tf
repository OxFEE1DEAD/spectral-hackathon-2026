terraform {
  required_version = ">= 1.5"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
  }
}

variable "region" {
  description = "AWS region hosting both benchmark instances."
  type        = string
  default     = "eu-central-1"
}

variable "sender_instance_type" {
  description = "Instance type for the host running producer and sender."
  type        = string
  default     = "m7i.2xlarge"
}

variable "receiver_instance_type" {
  description = "Instance type for the host running receiver and consumer."
  type        = string
  default     = "m7i.2xlarge"
}

variable "ssh_public_key" {
  description = "OpenSSH public key material authorised on both instances."
  type        = string

  validation {
    condition     = can(regex("^(ssh-(rsa|ed25519)|ecdsa-sha2-nistp[0-9]+) ", var.ssh_public_key))
    error_message = "ssh_public_key must be OpenSSH public key material, not a file path."
  }
}

variable "ssh_cidr" {
  description = "CIDR block allowed to reach port 22."
  type        = string

  validation {
    condition     = can(cidrnetmask(var.ssh_cidr))
    error_message = "ssh_cidr must be a valid IPv4 CIDR block, for example 203.0.113.7/32."
  }
}

variable "kernel_extra_cmdline" {
  description = "Extra kernel parameters appended to GRUB_CMDLINE_LINUX_DEFAULT. Empty matches the reference environment."
  type        = string
  default     = ""
}

variable "raise_socket_buffers" {
  description = "Raise net.core.rmem_max and wmem_max. The reference environment leaves them at kernel defaults."
  type        = bool
  default     = false
}

variable "project" {
  description = "Name prefix for every created resource."
  type        = string
  default     = "spectral"
}

provider "aws" {
  region = var.region
}

locals {
  physical_cores = {
    "m7i.large"    = 1
    "m7i.xlarge"   = 2
    "m7i.2xlarge"  = 4
    "m7i.4xlarge"  = 8
    "m7i.8xlarge"  = 16
    "m7i.12xlarge" = 24
    "m7i.16xlarge" = 32
    "m7i.24xlarge" = 48
    "m7i.48xlarge" = 96
  }

  hosts = {
    sender   = var.sender_instance_type
    receiver = var.receiver_instance_type
  }

  isolated = {
    for role, type in local.hosts :
    role => join(",", range(1, lookup(local.physical_cores, type, 4)))
  }
}

data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = ["099720109477"]

  filter {
    name   = "name"
    values = ["ubuntu/images/hvm-ssd-gp3/ubuntu-noble-24.04-amd64-server-*"]
  }
}

data "aws_vpc" "default" {
  default = true
}

data "aws_subnets" "default" {
  filter {
    name   = "vpc-id"
    values = [data.aws_vpc.default.id]
  }
}

data "aws_subnet" "bench" {
  id = sort(tolist(data.aws_subnets.default.ids))[0]
}

resource "aws_key_pair" "bench" {
  key_name   = "${var.project}-bench"
  public_key = var.ssh_public_key
}

resource "aws_placement_group" "bench" {
  name     = "${var.project}-cluster"
  strategy = "cluster"
}

resource "aws_security_group" "bench" {
  name        = "${var.project}-bench"
  description = "Benchmark hosts: administrative SSH plus unrestricted traffic between members."
  vpc_id      = data.aws_vpc.default.id
}

resource "aws_vpc_security_group_ingress_rule" "ssh" {
  security_group_id = aws_security_group.bench.id
  description       = "Administrative SSH from the operator."
  cidr_ipv4         = var.ssh_cidr
  ip_protocol       = "tcp"
  from_port         = 22
  to_port           = 22
}

resource "aws_vpc_security_group_ingress_rule" "peer" {
  security_group_id            = aws_security_group.bench.id
  description                  = "Measured path between benchmark hosts."
  referenced_security_group_id = aws_security_group.bench.id
  ip_protocol                  = "-1"
}

resource "aws_vpc_security_group_egress_rule" "all" {
  security_group_id = aws_security_group.bench.id
  description       = "Package installation and outbound benchmark traffic."
  cidr_ipv4         = "0.0.0.0/0"
  ip_protocol       = "-1"
}

resource "aws_instance" "bench" {
  for_each = local.hosts

  ami                         = data.aws_ami.ubuntu.id
  instance_type               = each.value
  subnet_id                   = data.aws_subnet.bench.id
  vpc_security_group_ids      = [aws_security_group.bench.id]
  key_name                    = aws_key_pair.bench.key_name
  placement_group             = aws_placement_group.bench.name
  associate_public_ip_address = true
  user_data_replace_on_change = true

  user_data = templatefile("${path.module}/user_data.sh.tftpl", {
    isolated             = local.isolated[each.key]
    extra_cmdline        = var.kernel_extra_cmdline
    raise_socket_buffers = var.raise_socket_buffers
  })

  dynamic "cpu_options" {
    for_each = can(regex("metal", each.value)) ? [] : [1]
    content {
      core_count       = lookup(local.physical_cores, each.value, 4)
      threads_per_core = 1
    }
  }

  root_block_device {
    volume_type = "gp3"
    volume_size = 20
  }

  tags = {
    Name = "${var.project}-${each.key}"
    Role = each.key
  }
}

output "sender_public_ip" {
  description = "Public address of the sending host."
  value       = aws_instance.bench["sender"].public_ip
}

output "receiver_public_ip" {
  description = "Public address of the receiving host."
  value       = aws_instance.bench["receiver"].public_ip
}

output "sender_private_ip" {
  description = "Sending host address on the measured path."
  value       = aws_instance.bench["sender"].private_ip
}

output "receiver_private_ip" {
  description = "Receiving host address on the measured path, passed to bench.sh as --peer."
  value       = aws_instance.bench["receiver"].private_ip
}

output "sender_isolated_cores" {
  description = "Cores withheld from the scheduler on the sending host."
  value       = local.isolated["sender"]
}

output "receiver_isolated_cores" {
  description = "Cores withheld from the scheduler on the receiving host."
  value       = local.isolated["receiver"]
}

output "availability_zone" {
  description = "Zone holding both instances."
  value       = data.aws_subnet.bench.availability_zone
}
